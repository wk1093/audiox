#include "audio/effects/advanced_eq.hpp"
#include "defs.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audiox::effects {
namespace {

constexpr double kPi = 3.14159265358979323846;

double finiteOr(double value, double fallback) { return std::isfinite(value) ? value : fallback; }

AdvancedEqChain makeChain(const float *params) {
    AdvancedEqChain chain{};
    const double rate = SAMPLE_RATE;
    chain.gain = std::pow(10.0, std::clamp(finiteOr(params[0], 0), -60.0, 60.0) / 20.0);
    const unsigned count =
        std::min<unsigned>(kAdvancedEqMaxBands, unsigned(std::clamp(finiteOr(params[1], 0), 0.0, 32.0) + 0.5));
    for (unsigned band = 0; band < count; ++band) {
        const float *p = params + kAdvancedEqFirstBandParam + band * kAdvancedEqParamsPerBand;
        const double type = std::clamp(finiteOr(p[0], 0), 0.0, double(AEQ_FILTER_LAST));
        if (advancedEqDesign(uint8_t(type + 0.5), p[1], p[2], p[3], rate, &chain.bands[band]))
            chain.active[chain.activeCount++] = uint8_t(band);
    }
    return chain;
}

inline double processBiquad(AdvancedEqBiquad &f, double x) {
    const double y = f.b0 * x + f.z1;
    f.z1 = f.b1 * x - f.a1 * y + f.z2;
    f.z2 = f.b2 * x - f.a2 * y;
    return y;
}

inline double processChain(AdvancedEqChain &chain, double x) {
    for (uint8_t i = 0; i < chain.activeCount; ++i) x = processBiquad(chain.bands[chain.active[i]], x);
    return x * chain.gain;
}

} // namespace

bool advancedEqDesign(uint8_t type, double hz, double gainDb, double q, double sampleRate,
                      AdvancedEqBiquad *out) {
    *out = AdvancedEqBiquad{};
    if (type == AEQ_OFF || type > AEQ_FILTER_LAST || sampleRate < 2) return false;
    gainDb = std::clamp(finiteOr(gainDb, 0), -60.0, 60.0);
    if ((type == AEQ_PEAK || type == AEQ_LOW_SHELF || type == AEQ_HIGH_SHELF) && std::fabs(gainDb) < 1e-9)
        return false;
    hz = std::clamp(finiteOr(hz, 1000), 1.0, sampleRate * 0.49);
    q = std::clamp(finiteOr(q, 0.707), 0.01, 100.0);
    const double w0 = 2 * kPi * hz / sampleRate;
    const double c = std::cos(w0);
    const double alpha = std::sin(w0) / (2 * q);
    const double A = std::pow(10.0, gainDb / 40);
    const double sa = 2 * std::sqrt(A) * alpha;
    double b0, b1, b2, a0, a1, a2;
    switch (type) {
    case AEQ_PEAK:
        b0 = 1 + alpha * A; b1 = -2 * c; b2 = 1 - alpha * A;
        a0 = 1 + alpha / A; a1 = -2 * c; a2 = 1 - alpha / A;
        break;
    case AEQ_LOW_SHELF:
        b0 = A * ((A + 1) - (A - 1) * c + sa);
        b1 = 2 * A * ((A - 1) - (A + 1) * c);
        b2 = A * ((A + 1) - (A - 1) * c - sa);
        a0 = (A + 1) + (A - 1) * c + sa;
        a1 = -2 * ((A - 1) + (A + 1) * c);
        a2 = (A + 1) + (A - 1) * c - sa;
        break;
    case AEQ_HIGH_SHELF:
        b0 = A * ((A + 1) + (A - 1) * c + sa);
        b1 = -2 * A * ((A - 1) + (A + 1) * c);
        b2 = A * ((A + 1) + (A - 1) * c - sa);
        a0 = (A + 1) - (A - 1) * c + sa;
        a1 = 2 * ((A - 1) - (A + 1) * c);
        a2 = (A + 1) - (A - 1) * c - sa;
        break;
    case AEQ_LOW_PASS:
        b0 = (1 - c) / 2; b1 = 1 - c; b2 = (1 - c) / 2;
        a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
        break;
    case AEQ_HIGH_PASS:
        b0 = (1 + c) / 2; b1 = -(1 + c); b2 = (1 + c) / 2;
        a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
        break;
    case AEQ_BAND_PASS:
        b0 = alpha; b1 = 0; b2 = -alpha;
        a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
        break;
    case AEQ_NOTCH:
        b0 = 1; b1 = -2 * c; b2 = 1;
        a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
        break;
    default: // AEQ_ALL_PASS
        b0 = 1 - alpha; b1 = -2 * c; b2 = 1 + alpha;
        a0 = 1 + alpha; a1 = -2 * c; a2 = 1 - alpha;
        break;
    }
    out->b0 = b0 / a0;
    out->b1 = b1 / a0;
    out->b2 = b2 / a0;
    out->a1 = a1 / a0;
    out->a2 = a2 / a0;
    return true;
}

void resetAdvancedEq(AdvancedEqState *state) {
    if (state) *state = AdvancedEqState{};
}

bool advancedEqHasPendingAudio(const AdvancedEqState &state, float threshold) {
    const double limit = std::max(0.0f, std::isfinite(threshold) ? threshold : 1.0e-12f);
    const auto energy = [limit](const AdvancedEqChain &chain) {
        for (uint8_t i = 0; i < chain.activeCount; ++i) {
            const auto &band = chain.bands[chain.active[i]];
            if (!(std::fabs(band.z1) <= limit) || !(std::fabs(band.z2) <= limit)) return true;
        }
        return false;
    };
    return energy(state.active) || (state.transitioning && energy(state.transition));
}

#ifndef AUDIOX_DSP_RUNTIME_ONLY
// A float loop rather than memcmp: the native DSP linker does not import memcmp/bcmp.
static bool paramsChanged(const float *last, const float *params) {
    bool changed = false;
    for (unsigned i = 0; i < kAdvancedEqParamCount; ++i) changed |= last[i] != params[i];
    return changed;
}

void processAdvancedEq(const float *in, float *out, uint32_t frames, const float *params,
                       AdvancedEqState *state) {
    if (!state || !in || !out || !params || !frames) return;
    if (!state->initialized) {
        state->active = makeChain(params);
        memcpy(state->lastParams, params, sizeof(state->lastParams));
        state->initialized = true;
    } else if (paramsChanged(state->lastParams, params)) {
        memcpy(state->lastParams, params, sizeof(state->lastParams));
        // A change mid-crossfade commits the pending chain first; 10 ms fades keep edits click-free.
        if (state->transitioning) state->active = state->transition;
        AdvancedEqChain next = makeChain(params);
        for (uint8_t i = 0; i < state->active.activeCount; ++i) {
            const uint8_t band = state->active.active[i];
            next.bands[band].z1 = state->active.bands[band].z1;
            next.bands[band].z2 = state->active.bands[band].z2;
        }
        state->transition = next;
        state->transitionPosition = 0;
        state->transitionLength = std::max<uint32_t>(1U, SAMPLE_RATE / 100U);
        state->transitioning = true;
    }

    if (!state->transitioning && !state->active.activeCount) {
        const float gain = float(state->active.gain);
        for (uint32_t i = 0; i < frames; ++i) out[i] = std::isfinite(in[i]) ? in[i] * gain : 0.0f;
        return;
    }
    for (uint32_t i = 0; i < frames; ++i) {
        const double x = std::isfinite(in[i]) ? in[i] : 0.0;
        const double oldValue = processChain(state->active, x);
        if (!state->transitioning) {
            out[i] = float(oldValue);
            continue;
        }
        const double newValue = processChain(state->transition, x);
        const double progress = double(state->transitionPosition + 1U) / state->transitionLength;
        out[i] = float(oldValue + (newValue - oldValue) * std::min(progress, 1.0));
        if (++state->transitionPosition >= state->transitionLength) {
            state->active = state->transition;
            state->transitioning = false;
        }
    }
}
#endif

} // namespace audiox::effects
