#include "audio/effects/equalizer.hpp"
#include "defs.hpp"

#include <algorithm>
#include <cmath>

namespace audiox::effects {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kMinQ = 0.2f;
constexpr float kMaxQ = 10.0f;
constexpr float kMaxGainDb = 18.0f;

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

float clampValue(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}

float boundedFrequency(float value, float sampleRate, float fallback) {
    const float nyquistSafeMax = std::max(1.0f, sampleRate * 0.49f);
    return clampValue(finiteOr(value, fallback), 1.0f, nyquistSafeMax);
}

EqualizerParams sanitize(const EqualizerParams &params) {
    EqualizerParams result;
    result.lowHz = clampValue(finiteOr(params.lowHz, 150.0f), 40.0f, 1000.0f);
    result.lowGainDb = clampValue(finiteOr(params.lowGainDb, 0.0f), -kMaxGainDb, kMaxGainDb);
    result.midHz = clampValue(finiteOr(params.midHz, 1000.0f), 200.0f, 8000.0f);
    result.midGainDb = clampValue(finiteOr(params.midGainDb, 0.0f), -kMaxGainDb, kMaxGainDb);
    result.highHz = clampValue(finiteOr(params.highHz, 6000.0f), 1000.0f, 18000.0f);
    result.highGainDb = clampValue(finiteOr(params.highGainDb, 0.0f), -kMaxGainDb, kMaxGainDb);
    result.q = clampValue(finiteOr(params.q, 1.0f), kMinQ, kMaxQ);
    result.outputGain = clampValue(finiteOr(params.outputGain, 1.0f), 0.0f, 4.0f);
    return result;
}

EqualizerBiquad makePeak(float frequency, float gainDb, float q, float sampleRate) {
    EqualizerBiquad result{};
    if (std::fabs(gainDb) < 1.0e-7f || sampleRate < 2.0f) {
        return result;
    }

    const float omega = 2.0f * kPi * frequency / sampleRate;
    const float cosine = std::cos(omega);
    const float alpha = std::sin(omega) / (2.0f * q);
    const float amplitude = std::pow(10.0f, gainDb / 40.0f);
    const float a0 = 1.0f + alpha / amplitude;
    result.b0 = (1.0f + alpha * amplitude) / a0;
    result.b1 = (-2.0f * cosine) / a0;
    result.b2 = (1.0f - alpha * amplitude) / a0;
    result.a1 = (-2.0f * cosine) / a0;
    result.a2 = (1.0f - alpha / amplitude) / a0;
    return result;
}

EqualizerChain makeChain(const EqualizerParams &params, uint32_t sampleRate) {
    const float rate = static_cast<float>(sampleRate);
    EqualizerChain chain{};
    chain.bands[0] =
        makePeak(boundedFrequency(params.lowHz, rate, 150.0f), params.lowGainDb, params.q, rate);
    chain.bands[1] =
        makePeak(boundedFrequency(params.midHz, rate, 1000.0f), params.midGainDb, params.q, rate);
    chain.bands[2] =
        makePeak(boundedFrequency(params.highHz, rate, 6000.0f), params.highGainDb, params.q, rate);
    chain.outputGain = params.outputGain;
    return chain;
}

bool sameCoefficients(const EqualizerChain &a, const EqualizerChain &b) {
    if (a.outputGain != b.outputGain) {
        return false;
    }
    for (size_t i = 0; i < a.bands.size(); ++i) {
        const auto &left = a.bands[i];
        const auto &right = b.bands[i];
        if (left.b0 != right.b0 || left.b1 != right.b1 || left.b2 != right.b2 ||
            left.a1 != right.a1 || left.a2 != right.a2) {
            return false;
        }
    }
    return true;
}

float processChain(EqualizerChain *chain, float input) {
    float value = input;
    for (auto &band : chain->bands) {
        const float output = band.b0 * value + band.z1;
        band.z1 = band.b1 * value - band.a1 * output + band.z2;
        band.z2 = band.b2 * value - band.a2 * output;
        value = output;
    }
    return value * chain->outputGain;
}

bool isIdentity(const EqualizerParams &params) {
    return params.lowGainDb == 0.0f && params.midGainDb == 0.0f && params.highGainDb == 0.0f &&
           params.outputGain == 1.0f;
}

bool above(float value, float threshold) { return std::fabs(value) > threshold; }

} // namespace

void resetEqualizer(EqualizerState *state) {
    if (state) {
        *state = EqualizerState{};
    }
}

bool equalizerHasPendingAudio(const EqualizerState &state, float threshold) {
    threshold = std::max(0.0f, finiteOr(threshold, 1.0e-12f));
    const auto hasEnergy = [threshold](const EqualizerChain &chain) {
        for (const auto &band : chain.bands) {
            if (above(band.z1, threshold) || above(band.z2, threshold)) {
                return true;
            }
        }
        return false;
    };
    return hasEnergy(state.active) || (state.transitioning && hasEnergy(state.transition));
}

#ifndef AUDIOX_DSP_RUNTIME_ONLY
void processEqualizer(const float *in, float *out, uint32_t frames, float lowHz, float lowGainDb,
                      float midHz, float midGainDb, float highHz, float highGainDb, float q,
                      float outputGain, EqualizerState *state) {
    if (!state || !in || !out || frames == 0) {
        return;
    }

    EqualizerParams params;
    params.lowHz = lowHz;
    params.lowGainDb = lowGainDb;
    params.midHz = midHz;
    params.midGainDb = midGainDb;
    params.highHz = highHz;
    params.highGainDb = highGainDb;
    params.q = q;
    params.outputGain = outputGain;
    const EqualizerParams safeParams = sanitize(params);
    const uint32_t safeRate = std::max<uint32_t>(SAMPLE_RATE, 2U);
    if (!state->initialized) {
        state->active = makeChain(safeParams, safeRate);
        state->transition = state->active;
        state->sampleRate = safeRate;
        state->initialized = true;
    } else {
        const EqualizerChain desired = makeChain(safeParams, safeRate);
        const EqualizerChain &comparison = state->transitioning ? state->transition : state->active;
        if (state->sampleRate != safeRate || !sameCoefficients(comparison, desired)) {
            EqualizerChain newChain = desired;
            const EqualizerChain &source =
                state->transitioning && !sameCoefficients(state->active, desired)
                    ? state->transition
                    : state->active;
            for (size_t i = 0; i < newChain.bands.size(); ++i) {
                newChain.bands[i].z1 = source.bands[i].z1;
                newChain.bands[i].z2 = source.bands[i].z2;
            }
            state->transition = newChain;
            state->transitionPosition = 0;
            state->transitionLength = std::max<uint32_t>(1U, safeRate / 100U);
            state->transitioning =
                state->transitioning || !sameCoefficients(state->active, desired);
            if (!state->transitioning) {
                state->active = newChain;
            }
            state->sampleRate = safeRate;
        }
    }

    if (!state->transitioning && isIdentity(safeParams)) {
        for (uint32_t i = 0; i < frames; ++i) {
            out[i] = finiteOr(in[i], 0.0f);
        }
        for (auto &band : state->active.bands) {
            band.z1 = 0.0f;
            band.z2 = 0.0f;
        }
        return;
    }

    for (uint32_t i = 0; i < frames; ++i) {
        const float input = finiteOr(in[i], 0.0f);
        const float oldValue = processChain(&state->active, input);
        if (!state->transitioning) {
            out[i] = oldValue;
            continue;
        }

        const float newValue = processChain(&state->transition, input);
        const float progress = static_cast<float>(state->transitionPosition + 1U) /
                               static_cast<float>(state->transitionLength);
        out[i] = oldValue + (newValue - oldValue) * std::min(progress, 1.0f);
        if (++state->transitionPosition >= state->transitionLength) {
            state->active = state->transition;
            state->transitioning = false;
        }
    }
}

#endif
} // namespace audiox::effects
