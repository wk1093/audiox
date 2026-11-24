#include "audio/effects/stereo.hpp"

#include <algorithm>
#include <cmath>

namespace audiox::effects {
namespace {

constexpr double kPi = 3.14159265358979323846;

inline float clean(float x) { return std::isfinite(x) ? x : 0.0f; }
inline double flush(double x) { return std::fabs(x) < 1e-20 ? 0.0 : x; }

void configureCrossfeed(CrossfeedState *s, float fcut, float feed) {
    const double gbLo = feed * -5.0 / 6.0 - 3.0;
    const double gbHi = feed / 6.0 - 3.0;
    const double gLo = std::pow(10.0, gbLo / 20.0);
    const double gHi = 1.0 - std::pow(10.0, gbHi / 20.0);
    const double fcHi = fcut * std::pow(2.0, (gbLo - 20.0 * std::log10(gHi)) / 12.0);
    double x = std::exp(-2.0 * kPi * fcut / SAMPLE_RATE);
    s->b1Lo = x;
    s->a0Lo = gLo * (1.0 - x);
    x = std::exp(-2.0 * kPi * std::min(fcHi, SAMPLE_RATE * 0.49) / SAMPLE_RATE);
    s->b1Hi = x;
    s->a0Hi = 1.0 - gHi * (1.0 - x);
    s->a1Hi = -x;
    s->gain = 1.0 / (1.0 - gHi + gLo);
    s->fcut = fcut;
    s->feed = feed;
}

} // namespace

void resetCrossfeed(CrossfeedState *state) {
    if (state) *state = CrossfeedState{};
}

bool crossfeedHasPendingAudio(const CrossfeedState &s, float threshold) {
    const double limit = std::max(0.0f, std::isfinite(threshold) ? threshold : 1.0e-12f);
    for (double v : {s.lowL, s.lowR, s.highL, s.highR, s.prevL, s.prevR})
        if (!(std::fabs(v) <= limit)) return true;
    return false;
}

void resetMaximizer(MaximizerState *state) {
    if (state) *state = MaximizerState{};
}

bool maximizerHasPendingAudio(const MaximizerState &s, float threshold) {
    const float limit = std::max(0.0f, std::isfinite(threshold) ? threshold : 1.0e-12f);
    for (const auto &line : s.delay)
        for (float v : line)
            if (!(std::fabs(v) <= limit)) return true;
    return false;
}

#ifndef AUDIOX_DSP_RUNTIME_ONLY
void processCrossfeed(const StereoIo &io, uint32_t frames, const float *params,
                      CrossfeedState *s) {
    if (!s || !params || !io.in[0] || !io.in[1] || !io.out[0] || !io.out[1]) return;
    const float fcut = std::clamp(params[0], 300.0f, 2000.0f);
    const float feed = std::clamp(params[1], 1.0f, 15.0f);
    const double output = std::clamp(params[2], 0.0f, 4.0f);
    if (!s->initialized || s->fcut != fcut || s->feed != feed) configureCrossfeed(s, fcut, feed);
    s->initialized = true;
    const double gain = s->gain * output;
    for (uint32_t i = 0; i < frames; ++i) {
        const double l = clean(io.in[0][i]);
        const double r = clean(io.in[1][i]);
        s->lowL = flush(s->a0Lo * l + s->b1Lo * s->lowL);
        s->lowR = flush(s->a0Lo * r + s->b1Lo * s->lowR);
        s->highL = flush(s->a0Hi * l + s->a1Hi * s->prevL + s->b1Hi * s->highL);
        s->highR = flush(s->a0Hi * r + s->a1Hi * s->prevR + s->b1Hi * s->highR);
        s->prevL = l;
        s->prevR = r;
        const float outL = float((s->highL + s->lowR) * gain);
        const float outR = float((s->highR + s->lowL) * gain);
        io.out[0][i] = outL;
        io.out[1][i] = outR;
    }
}

void processMaximizer(const StereoIo &io, uint32_t frames, const float *params,
                      MaximizerState *s) {
    if (!s || !params || !io.in[0] || !io.in[1] || !io.out[0] || !io.out[1]) return;
    constexpr uint32_t L = kMaximizerLookahead;
    const float boost = std::pow(10.0f, -std::clamp(params[0], -30.0f, 0.0f) / 20.0f);
    const float ceiling = std::pow(10.0f, std::clamp(params[1], -30.0f, 0.0f) / 20.0f);
    const double releaseSamples = std::clamp(params[2], 1.0f, 1000.0f) * 0.001 * SAMPLE_RATE;
    const double release = std::exp(-1.0 / releaseSamples);
    if (!s->initialized) {
        for (float &v : s->smoothing) v = 1.0f;
        s->smoothingSum = L;
        s->envelope = 1;
        s->initialized = true;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const float l = clean(io.in[0][i]);
        const float r = clean(io.in[1][i]);
        const float peak = std::max(std::fabs(l), std::fabs(r)) * boost;
        const float required = peak > ceiling ? ceiling / peak : 1.0f;

        // Sliding minimum of the required gain over the lookahead window.
        const uint64_t n = s->sample++;
        while (s->minCount &&
               s->minValue[(s->minHead + s->minCount - 1) % L] >= required)
            --s->minCount;
        if (s->minCount == L) {
            s->minHead = (s->minHead + 1) % L;
            --s->minCount;
        }
        const uint32_t back = (s->minHead + s->minCount) % L;
        s->minValue[back] = required;
        s->minPosition[back] = n;
        ++s->minCount;
        while (s->minPosition[s->minHead] + L <= n) {
            s->minHead = (s->minHead + 1) % L;
            --s->minCount;
        }
        const double windowMin = s->minValue[s->minHead];
        s->envelope = std::min(windowMin, 1.0 - (1.0 - s->envelope) * release);

        // The average of the previous L envelopes never exceeds the gain needed by the
        // sample leaving the delay line, so output peaks stay below the ceiling.
        const uint32_t c = s->cursor;
        const double gain = s->smoothingSum / L;
        s->gain = gain;
        const float scale = float(gain * boost);
        const float outL = std::clamp(s->delay[0][c] * scale, -ceiling, ceiling);
        const float outR = std::clamp(s->delay[1][c] * scale, -ceiling, ceiling);
        s->smoothingSum += s->envelope - s->smoothing[c];
        s->smoothing[c] = float(s->envelope);
        s->delay[0][c] = l;
        s->delay[1][c] = r;
        s->cursor = c + 1 == L ? 0 : c + 1;
        if (!s->cursor) {
            double sum = 0;
            for (float v : s->smoothing) sum += v;
            s->smoothingSum = sum;
        }
        io.out[0][i] = outL;
        io.out[1][i] = outR;
    }
}
#endif

} // namespace audiox::effects
