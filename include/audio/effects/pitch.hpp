#pragma once

#include <cstdint>

namespace audiox::effects {

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
