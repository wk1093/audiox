#include "audio/effects/gain.hpp"

namespace audiox::effects {

void processGain(const float *in,
                 float *out,
                 uint32_t frames,
                 float gain) {
    if (!in || !out) {
        return;
    }

    if (gain < 0.0f) {
        gain = 0.0f;
    }
    if (gain > 4.0f) {
        gain = 4.0f;
    }

    for (uint32_t frame = 0; frame < frames; ++frame) {
        float s = in[frame] * gain;
        if (s > 1.0f) {
            s = 1.0f;
        }
        if (s < -1.0f) {
            s = -1.0f;
        }
        out[frame] = s;
    }
}

} // namespace audiox::effects
