#pragma once
#include <algorithm>
#include <cstdint>

namespace audiox::scenes {
// The audio thread alone owns this envelope. Control threads only set its target.
struct Fade {
    float gain = 1.0f;
    float next(bool muted, uint32_t sampleRate) {
        const float step = 1.0f / std::max(1U, sampleRate / 100U);
        gain = muted ? std::max(0.0f, gain - step) : std::min(1.0f, gain + step);
        return gain;
    }
};
}
