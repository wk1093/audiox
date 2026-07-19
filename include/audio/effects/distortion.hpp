#pragma once

#include <cstdint>

namespace audiox::effects {

void processDistortion(const float *in,
                       float *out,
                       uint32_t frames,
                       float drive,
                       float clip,
                       float output);

} // namespace audiox::effects
