#pragma once

#include <cstdint>

namespace audiox::effects {

void processPitch(const char *effectId,
                  uint8_t channel,
                  const float *in,
                  float *out,
                  uint32_t frames,
                  float inputGain,
                  float semitoneShift,
                  float mix,
                  float outputGain);

} // namespace audiox::effects
