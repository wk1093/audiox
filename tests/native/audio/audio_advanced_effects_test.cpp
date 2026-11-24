#include "audio/effects/advanced_eq.hpp"
#include "audio/effects/apo.hpp"
#include "audio/effects/bass_enhancer.hpp"
#include "audio/effects/state_bank.hpp"
#include "audio/effects/stereo.hpp"
#include "defs.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace audiox::effects;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr float kRate = static_cast<float>(SAMPLE_RATE);

std::vector<float> tone(uint32_t frames, float hz, float amplitude) {
    std::vector<float> out(frames);
    for (uint32_t i = 0; i < frames; ++i) out[i] = amplitude * float(std::sin(2 * kPi * hz * i / kRate));
    return out;
}

double amplitudeAt(const std::vector<float> &x, uint32_t first, float hz) {
    double re = 0, im = 0;
    for (uint32_t i = first; i < x.size(); ++i) {
        re += x[i] * std::sin(2 * kPi * hz * i / kRate);
        im += x[i] * std::cos(2 * kPi * hz * i / kRate);
    }
    return 2 * std::hypot(re, im) / double(x.size() - first);
}

double db(double ratio) { return 20 * std::log10(ratio); }

void defaultAdvancedEq(float *p, unsigned bands) {
    std::fill(p, p + kAdvancedEqParamCount, 0.0f);
    p[1] = float(bands);
    for (unsigned b = 0; b < kAdvancedEqMaxBands; ++b) {
        float *band = p + kAdvancedEqFirstBandParam + b * kAdvancedEqParamsPerBand;
        band[0] = AEQ_PEAK;
        band[1] = 1000;
        band[2] = 0;
        band[3] = 1.41f;
    }
}

std::vector<float> runAdvancedEq(const float *params, const std::vector<float> &in) {
    auto state = std::make_unique<AdvancedEqState>();
    std::vector<float> out(in.size());
    for (size_t off = 0; off < in.size(); off += 128) {
        const uint32_t n = uint32_t(std::min<size_t>(128, in.size() - off));
        processAdvancedEq(in.data() + off, out.data() + off, n, params, state.get());
    }
    return out;
}

void testAdvancedEq() {
    float p[kAdvancedEqParamCount];
    defaultAdvancedEq(p, 0);
    const auto in = tone(SAMPLE_RATE / 2, 1000, 0.25f);
    auto out = runAdvancedEq(p, in);
    for (size_t i = 0; i < in.size(); ++i) assert(out[i] == in[i]);

    // Bands beyond the count are ignored; band 1 boosts 1 kHz by 6 dB.
    defaultAdvancedEq(p, 1);
    p[kAdvancedEqFirstBandParam + 2] = 6;
    p[kAdvancedEqFirstBandParam + 4 + 2] = -20;
    out = runAdvancedEq(p, in);
    assert(std::fabs(db(amplitudeAt(out, SAMPLE_RATE / 10, 1000) / 0.25) - 6) < 0.1);

    p[0] = -6;
    out = runAdvancedEq(p, in);
    assert(std::fabs(db(amplitudeAt(out, SAMPLE_RATE / 10, 1000) / 0.25)) < 0.1);

    // A high-pass at 1 kHz leaves 10 kHz alone and removes 50 Hz.
    defaultAdvancedEq(p, 1);
    p[kAdvancedEqFirstBandParam] = AEQ_HIGH_PASS;
    p[kAdvancedEqFirstBandParam + 3] = 0.7071f;
    assert(std::fabs(db(amplitudeAt(runAdvancedEq(p, tone(SAMPLE_RATE / 2, 10000, 0.25f)), SAMPLE_RATE / 10,
                                    10000) / 0.25)) < 0.2);
    assert(db(amplitudeAt(runAdvancedEq(p, tone(SAMPLE_RATE / 2, 50, 0.25f)), SAMPLE_RATE / 10, 50) / 0.25) < -40);
}

void testApo() {
    const char text[] =
        "# AutoEQ\r\n"
        "Preamp: -6,5 dB\n"
        "Filter 1: ON LSC Fc 105 Hz Gain 5.5 dB Q 0.70\n"
        "Filter 2: ON PK Fc 2000 Hz Gain -3 dB Q 1.5\n"
        "Filter 3: OFF PK Fc 3000 Hz Gain 2 dB Q 1\n"
        "Filter 4: ON HS 12 dB Fc 8000 Hz Gain 4 dB\n"
        "Filter: ON PK Fc 500 Hz Gain 2 dB BW Oct 1.0\n"
        "Filter 6: ON HP Fc 20 Hz\n"
        "Channel: R\n"
        "Filter 7: ON PK Fc 100 Hz Gain 10 dB Q 1\n"
        "Channel: all\n"
        "Delay: 10 ms\n"
        "Filter 8: ON NO Fc 60 Hz\n";
    ApoImport import;
    assert(parseApo(text, sizeof(text) - 1, &import));
    assert(std::fabs(import.preampDb + 6.5f) < 1e-5f);
    assert(import.bandCount == 7);
    assert(import.bands[0].type == AEQ_LOW_SHELF && import.bands[0].hz == 105 && import.bands[0].gainDb == 5.5f);
    assert(import.bands[1].type == AEQ_PEAK && import.bands[1].q == 1.5f);
    assert(import.bands[2].type == AEQ_OFF);
    assert(import.bands[3].type == AEQ_HIGH_SHELF);
    // A corner-frequency shelf is converted to the RBJ centre (below the corner for a high shelf).
    assert(import.bands[3].hz < 8000);
    assert(std::fabs(import.bands[4].q - 1.414f) < 0.01f);
    assert(import.bands[5].type == AEQ_HIGH_PASS);
    assert(import.bands[6].type == AEQ_NOTCH && import.bands[6].hz == 60);
    assert(!import.warnings.empty());

    float values[kAdvancedEqParamCount];
    defaultAdvancedEq(values, 32);
    applyApo(import, values);
    assert(values[1] == 7 && std::fabs(values[0] + 6.5f) < 1e-5f);

    const std::string exported = exportApo(values);
    ApoImport round;
    assert(parseApo(exported.data(), exported.size(), &round));
    assert(round.bandCount == import.bandCount && std::fabs(round.preampDb - import.preampDb) < 1e-4f);
    for (unsigned i = 0; i < round.bandCount; ++i) {
        assert(round.bands[i].type == import.bands[i].type);
        assert(std::fabs(round.bands[i].hz - import.bands[i].hz) < 0.05f);
        assert(std::fabs(round.bands[i].gainDb - import.bands[i].gainDb) < 1e-3f);
        assert(std::fabs(round.bands[i].q - import.bands[i].q) < 1e-3f);
    }

    ApoImport empty;
    assert(!parseApo("Device: all\n", 12, &empty));

    std::string many;
    for (int i = 0; i < 40; ++i) many += "Filter: ON PK Fc 1000 Hz Gain 1 dB Q 1\n";
    ApoImport capped;
    assert(parseApo(many.data(), many.size(), &capped));
    assert(capped.bandCount == kAdvancedEqMaxBands && capped.skipped == 8);
}

void testBassEnhancer() {
    float p[] = {6, 8.5f, 0, 100, 20, 0, 0, 1};
    const auto in = tone(SAMPLE_RATE / 2, 60, 0.3f);
    std::vector<float> out(in.size());
    auto state = std::make_unique<BassEnhancerState>();
    processBassEnhancer(in.data(), out.data(), uint32_t(in.size()), p, state.get());
    // Harmonics appear at 120/180 Hz that are not in the input.
    const double harmonics = amplitudeAt(out, SAMPLE_RATE / 10, 120) + amplitudeAt(out, SAMPLE_RATE / 10, 180);
    assert(harmonics > 0.003);
    for (float v : out) assert(std::isfinite(v));

    // Silence in, silence out (no DC thump from the saturator bias).
    resetBassEnhancer(state.get());
    std::vector<float> silent(4096, 0.0f), quiet(4096);
    processBassEnhancer(silent.data(), quiet.data(), 4096, p, state.get());
    for (float v : quiet) assert(std::fabs(v) < 1e-6f);

    // Listen mode outputs only harmonics, so a 5 kHz tone above the scope vanishes.
    p[6] = 1;
    resetBassEnhancer(state.get());
    const auto high = tone(SAMPLE_RATE / 4, 5000, 0.3f);
    std::vector<float> highOut(high.size());
    processBassEnhancer(high.data(), highOut.data(), uint32_t(high.size()), p, state.get());
    assert(amplitudeAt(highOut, SAMPLE_RATE / 20, 5000) < 0.003);
}

void testBassEnhancerStateBank() {
    auto bank = std::make_unique<StateBank<BassEnhancerState>>();
    auto *first = bank->find("first", 0);
    auto *second = bank->find("second", 0);
    second->initialized = true;
    second->preLow[0].b0 = 5;
    second->preLow[0].z1 = 7;
    for (unsigned i = 0; i < 126; ++i) {
        const std::string id = "other" + std::to_string(i);
        assert(bank->find(id.c_str(), 0));
    }
    assert(bank->find("first", 0) == first);
    assert(bank->find("missing", 0, false) == nullptr);
    auto *replacement = bank->find("replacement", 0);
    assert(replacement == second);
    assert(!replacement->initialized);
    assert(replacement->preLow[0].b0 == 1);
    assert(replacement->preLow[0].z1 == 0);
    assert(bank->find("second", 0, false) == nullptr);
}

void testCrossfeed() {
    const float p[] = {700, 4.5f, 1};
    const auto left = tone(SAMPLE_RATE / 2, 200, 0.3f);
    std::vector<float> zero(left.size(), 0.0f), outL(left.size()), outR(left.size());
    auto state = std::make_unique<CrossfeedState>();
    processCrossfeed({{left.data(), zero.data()}, {outL.data(), outR.data()}}, uint32_t(left.size()), p, state.get());
    // Low frequencies bleed into the silent right channel, below the direct level.
    const double direct = amplitudeAt(outL, SAMPLE_RATE / 10, 200);
    const double bleed = amplitudeAt(outR, SAMPLE_RATE / 10, 200);
    assert(bleed > 0.05 * direct && bleed < direct);

    // Mono content stays mono, and in-place processing works.
    resetCrossfeed(state.get());
    std::vector<float> l = left, r = left;
    processCrossfeed({{l.data(), r.data()}, {l.data(), r.data()}}, uint32_t(l.size()), p, state.get());
    for (size_t i = 0; i < l.size(); ++i) assert(std::fabs(l[i] - r[i]) < 1e-6f);
}

void testMaximizer() {
    const float p[] = {-12, -1, 25};
    const float ceiling = std::pow(10.0f, -1.0f / 20.0f);
    std::vector<float> l(SAMPLE_RATE), r(SAMPLE_RATE), outL(l.size()), outR(r.size());
    uint32_t seed = 1;
    for (size_t i = 0; i < l.size(); ++i) {
        seed = seed * 1664525u + 1013904223u;
        const float noise = float(int32_t(seed)) / 2147483648.0f;
        l[i] = 0.4f * float(std::sin(2 * kPi * 80 * i / kRate)) + 0.3f * noise;
        r[i] = (i % 4800 == 0) ? 1.0f : 0.1f * noise;
    }
    auto state = std::make_unique<MaximizerState>();
    for (size_t off = 0; off < l.size(); off += 64) {
        const uint32_t n = uint32_t(std::min<size_t>(64, l.size() - off));
        processMaximizer({{l.data() + off, r.data() + off}, {outL.data() + off, outR.data() + off}}, n, p,
                         state.get());
    }
    float peak = 0;
    for (size_t i = 0; i < l.size(); ++i) peak = std::max({peak, std::fabs(outL[i]), std::fabs(outR[i])});
    assert(peak <= ceiling + 1e-6f);
    assert(peak > 0.8f * ceiling);

    // Quiet input is boosted by -threshold once the lookahead latency has passed.
    resetMaximizer(state.get());
    const auto quiet = tone(SAMPLE_RATE / 4, 1000, 0.01f);
    std::vector<float> qL(quiet.size()), qR(quiet.size());
    processMaximizer({{quiet.data(), quiet.data()}, {qL.data(), qR.data()}}, uint32_t(quiet.size()), p, state.get());
    assert(std::fabs(db(amplitudeAt(qL, SAMPLE_RATE / 20, 1000) / 0.01) - 12) < 0.2);
}

} // namespace

int main() {
    testAdvancedEq();
    testApo();
    testBassEnhancer();
    testBassEnhancerStateBank();
    testCrossfeed();
    testMaximizer();
    std::puts("advanced EQ, APO, bass enhancer, crossfeed and maximizer tests passed");
    return 0;
}
