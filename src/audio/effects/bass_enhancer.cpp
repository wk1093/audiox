#include "audio/effects/bass_enhancer.hpp"
#include "defs.hpp"

#include <algorithm>
#include <cmath>

namespace audiox::effects {
namespace {

constexpr double kPi = 3.14159265358979323846;

float root(float x) {
    x = std::fabs(x);
    return x > 1e-9f ? std::sqrt(x) : 0.0f;
}

double flush(double x) { return std::fabs(x) < 1e-20 ? 0.0 : x; }

void design(BassBiquad *filter, double hz, bool highPass) {
    const double w0 = 2 * kPi * std::clamp(hz, 1.0, SAMPLE_RATE * 0.49) / SAMPLE_RATE;
    const double c = std::cos(w0);
    const double alpha = std::sin(w0) / (2 * 0.707);
    const double a0 = 1 + alpha;
    const double edge = highPass ? (1 + c) / 2 : (1 - c) / 2;
    filter->b0 = edge / a0;
    filter->b1 = (highPass ? -(1 + c) : 1 - c) / a0;
    filter->b2 = edge / a0;
    filter->a1 = -2 * c / a0;
    filter->a2 = (1 - alpha) / a0;
}

void configureSaturator(BassSaturator *s, float blend, float drive) {
    const float rdrive = 12.0f / std::max(drive, 0.1f);
    const float rbdr = rdrive / (10.5f - std::clamp(blend, -10.0f, 10.0f)) * 780.0f / 33.0f;
    s->kpa = root(2.0f * rdrive * rdrive - 1.0f) + 1.0f;
    s->kpb = (2.0f - s->kpa) / 2.0f;
    s->ap = (rdrive * rdrive - s->kpa + 1.0f) / 2.0f;
    s->kc = s->kpa / root(2.0f * root(2.0f * rdrive * rdrive - 1.0f) - 2.0f * rdrive * rdrive);
    s->srct = (0.1f * SAMPLE_RATE) / (0.1f * SAMPLE_RATE + 1.0f);
    s->sq = s->kc * s->kc + 1.0f;
    s->knb = -rbdr / root(s->sq);
    s->kna = 2.0f * s->kc * rbdr / root(s->sq);
    s->an = rbdr * rbdr / s->sq;
    s->imr = 2.0f * s->knb + root(2.0f * s->kna + 4.0f * s->an - 1.0f);
    s->pwrq = 2.0f / (s->imr + 1.0f);
    // Shift the DC blocker's history by the new zero-input operating point so
    // initialization and drive/blend edits do not emit a low-frequency step.
    const float rest = (root(s->ap) + s->kpb) * s->pwrq;
    s->prevMed += rest - s->rest;
    s->rest = rest;
}

inline double saturate(BassSaturator &s, float x) {
    const float med = x >= 0.0f ? (root(s.ap + x * (s.kpa - x)) + s.kpb) * s.pwrq
                                : -(root(s.an - x * (s.kna + x)) + s.knb) * s.pwrq;
    const double y = s.srct * (med - s.prevMed + s.prevOut);
    s.prevMed = flush(med);
    s.prevOut = flush(y);
    return y;
}

inline double run(BassBiquad &f, double x) {
    const double y = f.b0 * x + f.z1;
    f.z1 = flush(f.b1 * x - f.a1 * y + f.z2);
    f.z2 = flush(f.b2 * x - f.a2 * y);
    return y;
}

bool quiet(const BassBiquad &f, double limit) {
    return std::fabs(f.z1) <= limit && std::fabs(f.z2) <= limit;
}

} // namespace

void resetBassEnhancer(BassEnhancerState *state) {
    if (state) *state = BassEnhancerState{};
}

bool bassEnhancerHasPendingAudio(const BassEnhancerState &state, float threshold) {
    const double limit = std::max(0.0f, std::isfinite(threshold) ? threshold : 1.0e-12f);
    for (unsigned i = 0; i < 2; ++i)
        if (!quiet(state.preLow[i], limit) || !quiet(state.preHigh[i], limit) ||
            !quiet(state.postLow[i], limit) || !quiet(state.postHigh[i], limit))
            return true;
    return !(std::fabs(state.saturator.prevOut) <= limit);
}

#ifndef AUDIOX_DSP_RUNTIME_ONLY
void processBassEnhancer(const float *in, float *out, uint32_t frames, const float *params,
                         BassEnhancerState *state) {
    if (!in || !out || !params || !state || !frames) return;
    const float amount = std::pow(10.0f, std::clamp(params[0], -60.0f, 40.0f) / 20.0f);
    const float drive = std::clamp(params[1], 0.1f, 10.0f);
    const float blend = std::clamp(params[2], -10.0f, 10.0f);
    const float scope = std::clamp(params[3], 10.0f, 250.0f);
    const float floorHz = std::clamp(params[4], 10.0f, 120.0f);
    const bool floorActive = params[5] > 0.5f;
    const bool listen = params[6] > 0.5f;
    const float outputGain = std::clamp(params[7], 0.0f, 4.0f);
    if (!state->initialized || state->scopeHz != scope) {
        for (unsigned i = 0; i < 2; ++i) {
            design(&state->preLow[i], scope, false);
            design(&state->postLow[i], scope, false);
        }
        state->scopeHz = scope;
    }
    if (!state->initialized || state->floorHz != floorHz) {
        for (unsigned i = 0; i < 2; ++i) {
            design(&state->preHigh[i], floorHz, true);
            design(&state->postHigh[i], floorHz, true);
        }
        state->floorHz = floorHz;
    }
    if (!state->initialized || state->drive != drive || state->blend != blend) {
        configureSaturator(&state->saturator, blend, drive);
        state->drive = drive;
        state->blend = blend;
    }
    state->initialized = true;

    for (uint32_t i = 0; i < frames; ++i) {
        const float dry = std::isfinite(in[i]) ? in[i] : 0.0f;
        double x = run(state->preLow[1], run(state->preLow[0], dry));
        if (floorActive) x = run(state->preHigh[1], run(state->preHigh[0], x));
        x = saturate(state->saturator, float(x));
        x = run(state->postLow[1], run(state->postLow[0], x));
        if (floorActive) x = run(state->postHigh[1], run(state->postHigh[0], x));
        const double wet = x * amount;
        const float y = float((listen ? wet : dry + wet) * outputGain);
        out[i] = std::isfinite(y) ? y : 0.0f;
    }
}
#endif

} // namespace audiox::effects
