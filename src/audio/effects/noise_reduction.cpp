#include "audio/effects/noise_reduction.hpp"

#include "defs.hpp"
#include "audio/effects/fft.hpp"

#include <algorithm>
#include <cmath>

namespace audiox::effects {
namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kOlaNormalization = 2.0f;

float finiteOr(float value, float fallback) { return std::isfinite(value) ? value : fallback; }

float clampValue(float value, float low, float high) {
    return std::max(low, std::min(value, high));
}

NoiseReductionParams sanitize(const NoiseReductionParams &params) {
    NoiseReductionParams result;
    result.reductionDb = clampValue(finiteOr(params.reductionDb, 12.0f), 0.0f, 30.0f);
    result.noiseFloorDb = clampValue(finiteOr(params.noiseFloorDb, -55.0f), -90.0f, -20.0f);
    result.adaptation = clampValue(finiteOr(params.adaptation, 0.1f), 0.0f, 1.0f);
    result.smoothing = clampValue(finiteOr(params.smoothing, 0.7f), 0.0f, 1.0f);
    result.mix = clampValue(finiteOr(params.mix, 1.0f), 0.0f, 1.0f);
    result.outputGain = clampValue(finiteOr(params.outputGain, 1.0f), 0.0f, 4.0f);
    return result;
}

void fft(float *data, uint32_t n, bool inverse) {
    fftComplex(data, n, inverse);
    if (inverse) {
        const float scale = 1.0f / static_cast<float>(n);
        for (uint32_t i = 0; i < n * 2U; ++i) {
            data[i] *= scale;
        }
    }
}

void initialize(NoiseReductionState *state, const NoiseReductionParams &params) {
    for (uint32_t i = 0; i < NoiseReductionState::kFftSize; ++i) {
        const float phase = 2.0f * kPi * (static_cast<float>(i) + 0.5f) /
                            static_cast<float>(NoiseReductionState::kFftSize);
        state->window[i] = std::sqrt(0.5f - 0.5f * std::cos(phase));
    }
    const float floor = std::pow(10.0f, params.noiseFloorDb / 20.0f);
    state->noiseMagnitude.fill(floor);
    state->binGain.fill(1.0f);
    state->smoothedReductionDb = params.reductionDb;
    state->smoothedMix = params.mix;
    state->smoothedOutputGain = params.outputGain;
    state->initialized = true;
}

void processSpectrum(NoiseReductionState *state, int64_t frameStart,
                     const NoiseReductionParams &params) {
    constexpr uint32_t fftSize = NoiseReductionState::kFftSize;
    constexpr uint32_t binCount = NoiseReductionState::kBinCount;
    for (uint32_t i = 0; i < fftSize; ++i) {
        const int64_t inputIndex = frameStart + static_cast<int64_t>(i);
        const float sample =
            inputIndex < 0 ? 0.0f : state->inputRing[static_cast<uint64_t>(inputIndex) % fftSize];
        state->fftWork[i * 2U] = sample * state->window[i];
        state->fftWork[i * 2U + 1U] = 0.0f;
    }

    fft(state->fftWork.data(), fftSize, false);
    float spectrumMean = 0.0f;
    float logMagnitudeMean = 0.0f;
    for (uint32_t bin = 0; bin < binCount; ++bin) {
        const float real = state->fftWork[bin * 2U];
        const float imag = state->fftWork[bin * 2U + 1U];
        const float magnitude = std::sqrt(real * real + imag * imag) / static_cast<float>(fftSize);
        state->spectrumMagnitude[bin] = magnitude;
        spectrumMean += magnitude;
        logMagnitudeMean += std::log(std::max(magnitude, 1.0e-12f));
    }
    spectrumMean /= static_cast<float>(binCount);
    const float spectralFlatness = std::exp(logMagnitudeMean / static_cast<float>(binCount)) /
                                   std::max(spectrumMean, 1.0e-12f);
    const bool transient =
        state->previousSpectrumMean > 1.0e-9f && spectrumMean > state->previousSpectrumMean * 1.45f;
    const bool speechLike = spectralFlatness < 0.32f;
    state->previousSpectrumMean += 0.12f * (spectrumMean - state->previousSpectrumMean);

    const float reductionStrength = 1.0f - std::pow(10.0f, -state->smoothedReductionDb / 20.0f);
    const float minimumGain = 1.0f - reductionStrength;
    const float adaptation = params.adaptation;
    const float rise = 0.006f * adaptation;
    const float fall = adaptation == 0.0f ? 0.0f : 0.02f + 0.08f * adaptation;
    const float noiseFloorMinimum = std::pow(10.0f, params.noiseFloorDb / 20.0f);
    const float gainSmoothing = 1.0f - 0.98f * params.smoothing;

    for (uint32_t bin = 0; bin < binCount; ++bin) {
        const float magnitude = state->spectrumMagnitude[bin];
        float noise = state->noiseMagnitude[bin];
        const float left =
            bin == 0 ? state->spectrumMagnitude[bin + 1U] : state->spectrumMagnitude[bin - 1U];
        const float right = bin + 1U == binCount ? state->spectrumMagnitude[bin - 1U]
                                                 : state->spectrumMagnitude[bin + 1U];
        const bool tonalPeak = magnitude > (left + right) * 1.25f;
        const float alpha =
            magnitude < noise ? fall : ((transient || speechLike || tonalPeak) ? 0.0f : rise);
        noise += alpha * (magnitude - noise);
        noise = std::max(noiseFloorMinimum, noise);
        state->noiseMagnitude[bin] = noise;

        float targetGain = 1.0f;
        if (state->smoothedReductionDb > 0.0f && magnitude > 1.0e-12f) {
            targetGain =
                clampValue(1.0f - reductionStrength * noise / magnitude, minimumGain, 1.0f);
        } else if (state->smoothedReductionDb > 0.0f) {
            targetGain = minimumGain;
        }
        state->binGain[bin] += gainSmoothing * (targetGain - state->binGain[bin]);
        state->fftWork[bin * 2U] *= state->binGain[bin];
        state->fftWork[bin * 2U + 1U] *= state->binGain[bin];
    }

    for (uint32_t bin = 1; bin + 1U < binCount; ++bin) {
        const uint32_t mirror = fftSize - bin;
        state->fftWork[mirror * 2U] = state->fftWork[bin * 2U];
        state->fftWork[mirror * 2U + 1U] = -state->fftWork[bin * 2U + 1U];
    }
    state->fftWork[1] = 0.0f;
    state->fftWork[fftSize + 1U] = 0.0f;
    fft(state->fftWork.data(), fftSize, true);

    for (uint32_t i = 0; i < fftSize; ++i) {
        const int64_t inputIndex = frameStart + static_cast<int64_t>(i);
        if (inputIndex < 0) {
            continue;
        }
        const uint64_t outputIndex =
            static_cast<uint64_t>(inputIndex) + NoiseReductionState::kLatencyFrames;
        const uint32_t ringIndex =
            static_cast<uint32_t>(outputIndex % NoiseReductionState::kOutputRingSize);
        state->wetRing[ringIndex] += state->fftWork[i * 2U] * state->window[i] / kOlaNormalization;
    }
}

bool above(float value, float threshold) { return std::fabs(value) > threshold; }

} // namespace

void resetNoiseReduction(NoiseReductionState *state) {
    if (state) {
        *state = NoiseReductionState{};
    }
}

bool noiseReductionHasPendingAudio(const NoiseReductionState &state, float threshold) {
    threshold = std::max(0.0f, finiteOr(threshold, 1.0e-12f));
    for (float sample : state.dryDelay) {
        if (above(sample, threshold)) {
            return true;
        }
    }
    for (float sample : state.wetRing) {
        if (above(sample, threshold)) {
            return true;
        }
    }
    return false;
}

uint32_t noiseReductionLatencyFrames() { return NoiseReductionState::kLatencyFrames; }

#ifndef AUDIOX_DSP_RUNTIME_ONLY
void processNoiseReduction(const float *in, float *out, uint32_t frames, float reductionDb,
                           float noiseFloorDb, float adaptation, float smoothing, float mix,
                           float outputGain, NoiseReductionState *state) {
    if (!state || !in || !out || frames == 0) {
        return;
    }

    NoiseReductionParams params;
    params.reductionDb = reductionDb;
    params.noiseFloorDb = noiseFloorDb;
    params.adaptation = adaptation;
    params.smoothing = smoothing;
    params.mix = mix;
    params.outputGain = outputGain;
    const NoiseReductionParams safeParams = sanitize(params);
    if (!state->initialized) {
        initialize(state, safeParams);
    }

    for (uint32_t i = 0; i < frames; ++i) {
        const float input = finiteOr(in[i], 0.0f);
        const uint32_t outputIndex =
            static_cast<uint32_t>(state->samplePosition % NoiseReductionState::kOutputRingSize);
        const float delayedDry = state->dryDelay[state->dryPosition];
        state->dryDelay[state->dryPosition] = input;
        state->dryPosition = (state->dryPosition + 1U) % NoiseReductionState::kFftSize;

        state->inputRing[state->samplePosition % NoiseReductionState::kFftSize] = input;
        if (state->samplePosition >= NoiseReductionState::kHopSize - 1U &&
            (state->samplePosition - (NoiseReductionState::kHopSize - 1U)) %
                    NoiseReductionState::kHopSize ==
                0U) {
            const int64_t frameStart = static_cast<int64_t>(state->samplePosition) -
                                       static_cast<int64_t>(NoiseReductionState::kFftSize) + 1;
            const uint32_t smoothingFrames = 128U;
            state->smoothedReductionDb += (safeParams.reductionDb - state->smoothedReductionDb) /
                                          static_cast<float>(smoothingFrames);
            processSpectrum(state, frameStart, safeParams);
        }

        const float delayedWet = state->wetRing[outputIndex];
        state->wetRing[outputIndex] = 0.0f;

        const float smoothing = 1.0f / 128.0f;
        state->smoothedMix += smoothing * (safeParams.mix - state->smoothedMix);
        state->smoothedOutputGain +=
            smoothing * (safeParams.outputGain - state->smoothedOutputGain);
        if (safeParams.reductionDb == 0.0f && safeParams.mix == 1.0f &&
            safeParams.outputGain == 1.0f && state->smoothedReductionDb == 0.0f &&
            state->smoothedMix == 1.0f && state->smoothedOutputGain == 1.0f) {
            out[i] = delayedDry;
        } else {
            const float mixed = delayedDry + (delayedWet - delayedDry) * state->smoothedMix;
            out[i] = mixed * state->smoothedOutputGain;
        }
        ++state->samplePosition;
    }
}

#endif
} // namespace audiox::effects
