#pragma once

#include <array>
#include <cstdint>

namespace audiox::effects {

struct NoiseReductionParams {
    float reductionDb = 12.0f;
    float noiseFloorDb = -55.0f;
    float adaptation = 0.1f;
    float smoothing = 0.7f;
    float mix = 1.0f;
    float outputGain = 1.0f;
};

struct NoiseReductionState {
    static constexpr uint32_t kFftSize = 256;
    static constexpr uint32_t kHopSize = 64;
    static constexpr uint32_t kBinCount = kFftSize / 2U + 1U;
    static constexpr uint32_t kLatencyFrames = kFftSize;
    static constexpr uint32_t kOutputRingSize = 1024;

    std::array<float, kFftSize> inputRing{};
    std::array<float, kFftSize * 2U> fftWork{};
    std::array<float, kFftSize> window{};
    std::array<float, kOutputRingSize> wetRing{};
    std::array<float, kFftSize> dryDelay{};
    std::array<float, kBinCount> noiseMagnitude{};
    std::array<float, kBinCount> spectrumMagnitude{};
    std::array<float, kBinCount> binGain{};
    uint64_t samplePosition = 0;
    uint32_t dryPosition = 0;
    float previousSpectrumMean = 0.0f;
    float smoothedReductionDb = 0.0f;
    float smoothedMix = 1.0f;
    float smoothedOutputGain = 1.0f;
    bool initialized = false;
};

void resetNoiseReduction(NoiseReductionState *state);
bool noiseReductionHasPendingAudio(const NoiseReductionState &state, float threshold = 1.0e-12f);
uint32_t noiseReductionLatencyFrames();
// Phase-preserving spectral suppression; fixed 256-frame latency, 64-frame hop.
void processNoiseReduction(const float *in, float *out, uint32_t frames, float reductionDb,
                           float noiseFloorDb, float adaptation, float smoothing, float mix,
                           float outputGain, NoiseReductionState *state);

} // namespace audiox::effects
