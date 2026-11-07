#pragma once

#include <cstdint>
#include "audio/effects/fft.hpp"

namespace audiox::effects {

struct PitchState {
    static constexpr uint32_t FFT_SIZE = 1024;
    static constexpr uint32_t HALF_BINS = FFT_SIZE / 2U + 1U;
    float inFifo[FFT_SIZE]{}, outFifo[FFT_SIZE]{}, fftWork[FFT_SIZE * 2U]{};
    float outputAccum[FFT_SIZE]{}, dryDelay[FFT_SIZE]{};
    uint32_t dryIndex{};
    float lastPhase[HALF_BINS]{}, sumPhase[HALF_BINS]{}, anaMag[HALF_BINS]{};
    float anaFreq[HALF_BINS]{}, synMag[HALF_BINS]{}, synFreqAcc[HALF_BINS]{};
    uint32_t rover{};
    float wetLpfZ{};
    bool inited{};
    uint8_t quality{};
    float cachedShift{}, pitchRatio{}, cachedLpfHz{}, wetLpfAlpha{};
};

struct PitchPlan {
    FftPlan<PitchState::FFT_SIZE> fft;
    float window[PitchState::FFT_SIZE]{};
    float synthesis[3][PitchState::FFT_SIZE]{};
    PitchPlan();
};
PitchState *pitchStateFor(const char *, uint8_t, bool create = true);
const PitchPlan &getPitchPlan();
void processPitchState(PitchState *, const PitchPlan &, const float *, float *, uint32_t,
                       float, float, float, float, float, float, uint8_t);

bool pitchHasPendingAudio(const char *effectId, uint8_t channel, float threshold);
void resetPitch(const char *effectId, uint8_t channel);

void processPitch(const char *effectId,
                  uint8_t channel,
                  const float *in,
                  float *out,
                  uint32_t frames,
                  float inputGain,
                  float semitoneShift,
                  float mix,
                  float outputGain,
                  float noiseFloorRatio,
                  float wetLpfHz,
                  uint8_t quality = 0);

} // namespace audiox::effects
