#include "audio/effects/distortion.hpp"

#include <cmath>

namespace audiox::effects {

void processDistortion(const float *in,
                       float *out,
                       uint32_t frames,
                       float drive,
                       float clip,
                       float output) {
    if (!in || !out) {
        return;
    }

    if (drive < 0.0f) {
        drive = 0.0f;
    }
    if (drive > 8.0f) {
        drive = 8.0f;
    }
    if (clip < 0.05f) {
        clip = 0.05f;
    }
    if (clip > 1.0f) {
        clip = 1.0f;
    }
    if (output < 0.0f) {
        output = 0.0f;
    }
    if (output > 4.0f) {
        output = 4.0f;
    }

    const float shapedDrive = 1.0f + (drive * 6.0f);
    for (uint32_t frame = 0; frame < frames; ++frame) {
        float s = in[frame] * shapedDrive;
        s = tanhf(s);

        if (s > clip) {
            s = clip;
        }
        if (s < -clip) {
            s = -clip;
        }

        s *= output;

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
