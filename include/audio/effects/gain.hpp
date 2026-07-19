#pragma once

#include <cstdint>

namespace audiox::effects {

void processGain(const float *in,
                 float *out,
                 uint32_t frames,
                 float gain);

} // namespace audiox::effects
