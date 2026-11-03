#include "audio/effects/dynamics.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace audiox::effects;

namespace {
constexpr double pi = 3.14159265358979323846;

double db(double value) { return 20 * std::log10(std::max(value, 1.0e-30)); }
double lin(double value) { return std::pow(10.0, value / 20); }
void near(double actual, double expected, double tolerance = 1.0e-5) {
    assert(std::abs(actual - expected) <= tolerance);
}
void compressor(const float *in, float *out, uint32_t n, CompressorState &s, float threshold = -18,
                float ratio = 4, float attack = 10, float release = 120, float knee = 6,
                float makeup = 0, float mix = 1) {
    processCompressor(in, out, n, threshold, ratio, attack, release, knee, makeup, mix, &s);
}
void limiter(const float *in, float *out, uint32_t n, LimiterState &s, float ceiling = -1,
             float release = 80, float lookahead = 3, float gain = 1) {
    processLimiter(in, out, n, ceiling, release, lookahead, gain, &s);
}
void multiband(const float *in, float *out, uint32_t n, MultibandCompressorState &s,
               float low = 200, float high = 2500, float lt = -18, float mt = -18, float ht = -18,
               float ratio = 4, float release = 120, float makeup = 0) {
    processMultibandCompressor(in, out, n, low, high, lt, mt, ht, ratio, release, makeup, &s);
}
double rms(const std::vector<float> &v, size_t start) {
    double energy = 0;
    for (size_t i = start; i < v.size(); ++i)
        energy += double(v[i]) * v[i];
    return std::sqrt(energy / (v.size() - start));
}
std::vector<float> signal(size_t count) {
    std::vector<float> v(count);
    uint32_t seed = 1927;
    for (float &x : v) {
        seed = seed * 1664525u + 1013904223u;
        x = (double(seed) / std::numeric_limits<uint32_t>::max() - 0.5) * 4;
    }
    return v;
}
struct ReferenceAllpass {
    double a1, a2, x1{}, x2{}, y1{}, y2{};
    explicit ReferenceAllpass(double hz) {
        const double g = std::tan(pi * hz / SAMPLE_RATE);
        const double denominator = 1 + std::sqrt(2.0) * g + g * g;
        a1 = 2 * (g * g - 1) / denominator;
        a2 = (1 - std::sqrt(2.0) * g + g * g) / denominator;
    }
    double process(double x) {
        const double y = a2 * x + a1 * x1 + x2 - a1 * y1 - a2 * y2;
        x2 = x1;
        x1 = x;
        y2 = y1;
        y1 = y;
        return y;
    }
};

void testCompressor() {
    const uint32_t second = SAMPLE_RATE;
    std::vector<float> in(second * 2, 1), out(in.size());
    CompressorState state{};
    compressor(in.data(), out.data(), in.size(), state, -20, 4, 10, 120, 0);
    near(db(out.back()), -15, 0.001);
    near(db(out[second / 100 - 1]), -15 * (1 - std::exp(-1.0)), 0.001);
    assert(out.front() > out[second / 100]);
    const double initial = state.reductionDb;
    std::fill(in.begin(), in.end(), 0.01f);
    compressor(in.data(), out.data(), second * 120 / 1000, state, -20, 4, 10, 120, 0);
    near(state.reductionDb, initial / std::exp(1.0), 0.003);
    assert(out[0] < out[second * 120 / 1000 - 1]);
    assert(!compressorHasPendingAudio(state));

    resetCompressor(&state);
    compressor(in.data(), out.data(), in.size(), state, -20, 4, 10, 120, 0);
    near(out.back(), 0.01);
    std::fill(in.begin(), in.end(), 0.1f);
    resetCompressor(&state);
    compressor(in.data(), out.data(), in.size(), state, -20, 4, 10, 120, 6);
    near(db(out.back()) - db(0.1), -0.5625, 0.001);
    std::fill(in.begin(), in.end(), 1);
    compressor(in.data(), out.data(), in.size(), state, -20, 4, 10, 120, 0, 6, 0.25);
    near(out.back(), 0.75 + 0.25 * lin(-9), 1.0e-5);
    compressor(in.data(), out.data(), in.size(), state, -20, 1, 10, 120, 6);
    assert(out == in);
    compressor(in.data(), out.data(), in.size(), state, -60, 20, 0.1, 1000, 24, 24, 0);
    assert(out == in);
    resetCompressor(&state);
    assert(state.reductionDb == 0);
}

void testPartitions() {
    const auto in = signal(10017);
    std::vector<float> whole(in.size()), split(in.size()), inplace(in);
    CompressorState c1{}, c2{}, c3{};
    LimiterState l1{}, l2{}, l3{};
    MultibandCompressorState m1{}, m2{}, m3{};
    for (unsigned effect = 0; effect < 3; ++effect) {
        auto run = [&](const float *src, float *dst, uint32_t n, unsigned which) {
            if (effect == 0)
                compressor(src, dst, n, which == 0 ? c1 : which == 1 ? c2 : c3);
            if (effect == 1)
                limiter(src, dst, n, which == 0 ? l1 : which == 1 ? l2 : l3);
            if (effect == 2)
                multiband(src, dst, n, which == 0 ? m1 : which == 1 ? m2 : m3);
        };
        run(in.data(), whole.data(), in.size(), 0);
        for (size_t offset = 0; offset < in.size();) {
            const auto n = std::min<size_t>(1 + offset % 191, in.size() - offset);
            run(in.data() + offset, split.data() + offset, n, 1);
            offset += n;
        }
        inplace = in;
        run(inplace.data(), inplace.data(), inplace.size(), 2);
        assert(whole == split);
        assert(whole == inplace);
    }
}

void testLimiter() {
    auto in = signal(SAMPLE_RATE / 2);
    std::vector<float> out(in.size());
    LimiterState s{};
    for (unsigned pass = 0; pass < 5; ++pass) {
        const float ceiling = -24 + 6.0f * pass;
        for (size_t offset = 0; offset < in.size();) {
            const auto n = std::min<size_t>(1 + offset % 139, in.size() - offset);
            limiter(in.data() + offset, out.data() + offset, n, s, ceiling, pass % 2 ? 5 : 1000,
                    float(offset % 11), 4);
            const float bound = static_cast<float>(lin(ceiling));
            for (size_t i = offset; i < offset + n; ++i) {
                assert(std::isfinite(out[i]));
                assert(std::abs(out[i]) <= bound);
                assert(std::abs(double(out[i])) <= lin(ceiling));
            }
            offset += n;
        }
    }
    resetLimiter(&s);
    std::fill(in.begin(), in.end(), 0);
    in[0] = 0.25;
    limiter(in.data(), out.data(), in.size(), s);
    const size_t delay = std::lround(3.0 * SAMPLE_RATE / 1000);
    near(out[delay], 0.25);
    for (size_t i = 0; i < out.size(); ++i)
        if (i != delay)
            assert(out[i] == 0);
    resetLimiter(&s);
    float peak = 4, result = 0;
    limiter(&peak, &result, 1, s, -6, 80, 0);
    near(result, lin(-6));
    const double before = s.gain;
    std::fill(in.begin(), in.end(), 0.1);
    limiter(in.data(), out.data(), SAMPLE_RATE * 80 / 1000, s, -6, 80, 0);
    near(s.gain, 1 - (1 - before) / std::exp(1.0), 0.0005);
    resetLimiter(&s);
    limiter(&peak, &result, 1, s);
    assert(limiterHasPendingAudio(s));
    assert(!limiterHasPendingAudio(s, 5));
    std::fill(in.begin(), in.end(), 0);
    limiter(in.data(), out.data(), LimiterState::kMaxLookaheadFrames + 1, s);
    assert(!limiterHasPendingAudio(s));
    resetLimiter(&s);
    assert(s.gain == 1 && s.writeIndex == 0 && !limiterHasPendingAudio(s));
    for (size_t i = 0; i <= delay; ++i) {
        const float input = i == 0 ? 1.0f : 0.0f;
        limiter(&input, &result, 1, s, -6, 5, 3);
        near(s.gain, lin(-6), 1e-6);
        near(result, i == delay ? lin(-6) : 0, 1e-6);
    }
}

void testMultiband() {
    auto in = signal(SAMPLE_RATE);
    std::vector<float> out(in.size());
    MultibandCompressorState s{};
    multiband(in.data(), out.data(), in.size(), s, 200, 2500, -18, -18, -18, 1);
    ReferenceAllpass low(200), high(std::min(2500.0, SAMPLE_RATE * 0.45));
    for (size_t i = 0; i < in.size(); ++i)
        near(out[i], high.process(low.process(in[i])), 1.0e-5);

    for (double frequency : {40.0, 200.0, 700.0, 2500.0, SAMPLE_RATE * 0.4}) {
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.7 * std::sin(2 * pi * frequency * i / SAMPLE_RATE);
        resetMultibandCompressor(&s);
        multiband(in.data(), out.data(), in.size(), s, 200, 2500, -18, -18, -18, 1);
        near(rms(out, in.size() / 2) / rms(in, in.size() / 2), 1, 0.002);
    }
    const double frequencies[] = {20, 700, SAMPLE_RATE * 0.4};
    for (unsigned band = 0; band < 3; ++band) {
        for (size_t i = 0; i < in.size(); ++i)
            in[i] = 0.9 * std::sin(2 * pi * frequencies[band] * i / SAMPLE_RATE);
        resetMultibandCompressor(&s);
        const float t[] = {band == 0 ? -30.0f : 0, band == 1 ? -30.0f : 0, band == 2 ? -30.0f : 0};
        multiband(in.data(), out.data(), in.size(), s, 200, 2500, t[0], t[1], t[2], 10);
        assert(rms(out, in.size() / 2) < rms(in, in.size() / 2) * 0.3);
        assert(s.reductionDb[band] > 12);
    }
    auto constrained = signal(4000);
    std::vector<float> other(constrained.size());
    out.resize(constrained.size());
    MultibandCompressorState s2{};
    resetMultibandCompressor(&s);
    multiband(constrained.data(), out.data(), out.size(), s, 5000, 500);
    multiband(constrained.data(), other.data(), other.size(), s2, 1000, 1100);
    assert(out == other);
    resetMultibandCompressor(&s);
    float impulse = 1, result = 0;
    multiband(&impulse, &result, 1, s, 200, 2500, -18, -18, -18, 1);
    assert(multibandCompressorHasPendingAudio(s));
    std::vector<float> silence(SAMPLE_RATE), tail(silence.size());
    multiband(silence.data(), tail.data(), silence.size(), s, 200, 2500, -18, -18, -18, 1);
    assert(rms(tail, 0) > 0);
    assert(!multibandCompressorHasPendingAudio(s));
    resetMultibandCompressor(&s);
    multiband(silence.data(), tail.data(), tail.size(), s);
    assert(tail == silence);
}

void testFiniteAndChanges() {
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    std::vector<float> in = signal(3000), out(in.size()), reference(in.size());
    in[0] = nan;
    in[1] = inf;
    in[2] = -inf;
    in[3] = std::numeric_limits<float>::max();
    in[4] = -in[3];
    CompressorState c{};
    LimiterState l{};
    MultibandCompressorState m{};
    for (size_t i = 0; i < in.size(); ++i) {
        const bool extreme = i % 2 == 0;
        compressor(&in[i], &out[i], 1, c, extreme ? -100 : 100, extreme ? 99 : 0,
                   extreme ? 0 : 1000, extreme ? 0 : 10000, extreme ? 0 : 100, extreme ? -100 : 100,
                   extreme ? 0 : 1);
        assert(std::isfinite(out[i]));
        limiter(&in[i], &out[i], 1, l, extreme ? -100 : 100, extreme ? 0 : 10000,
                extreme ? -100 : 100, extreme ? 0 : 100);
        assert(std::isfinite(out[i]) && std::abs(out[i]) <= (extreme ? float(lin(-24)) : 1));
        multiband(&in[i], &out[i], 1, m, extreme ? 0 : 1.0e6, extreme ? 0 : 1.0e6, -100, 100, -100,
                  extreme ? 0 : 100, extreme ? 0 : 10000, 100);
        assert(std::isfinite(out[i]));
    }
    in = signal(in.size());
    resetCompressor(&c);
    CompressorState c2{};
    compressor(in.data(), out.data(), in.size(), c, nan, inf, nan, inf, nan, inf, nan);
    compressor(in.data(), reference.data(), in.size(), c2);
    assert(out == reference);
    resetLimiter(&l);
    LimiterState l2{};
    limiter(in.data(), out.data(), in.size(), l, nan, inf, nan, inf);
    limiter(in.data(), reference.data(), in.size(), l2);
    assert(out == reference);
    resetMultibandCompressor(&m);
    MultibandCompressorState m2{};
    multiband(in.data(), out.data(), in.size(), m, nan, inf, nan, inf, nan, inf, nan, inf);
    multiband(in.data(), reference.data(), in.size(), m2);
    assert(out == reference);

    // Processing a loud channel must not change a separate silent channel.
    std::fill(in.begin(), in.end(), 0);
    CompressorState silentC{};
    LimiterState silentL{};
    MultibandCompressorState silentM{};
    compressor(in.data(), out.data(), in.size(), silentC);
    assert(in == out);
    limiter(in.data(), out.data(), in.size(), silentL);
    assert(in == out);
    multiband(in.data(), out.data(), in.size(), silentM);
    assert(in == out);
    processCompressor(nullptr, out.data(), 1, -18, 4, 10, 120, 6, 0, 1, &c);
    processLimiter(in.data(), nullptr, 1, -1, 80, 3, 1, &l);
    processMultibandCompressor(in.data(), out.data(), 1, 200, 2500, -18, -18, -18, 4, 120, 0,
                               nullptr);
    resetCompressor(nullptr);
    resetLimiter(nullptr);
    resetMultibandCompressor(nullptr);
}
} // namespace

int main() {
    testCompressor();
    testPartitions();
    testLimiter();
    testMultiband();
    testFiniteAndChanges();
    std::puts("Audio dynamics tests passed");
}
