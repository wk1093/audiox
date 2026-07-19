#include "audio/effects/pitch.hpp"

#include <cmath>

namespace audiox::effects {

void processPitch(const float *in,
                  float *out,
                  uint32_t frames,
                  float inputGain,
                  float semitoneShift,
                  float mix,
                  float outputGain) {
    if (!in || !out || frames == 0) {
        return;
    }

    if (inputGain < 0.0f) {
        inputGain = 0.0f;
    }
    if (inputGain > 4.0f) {
        inputGain = 4.0f;
    }

    if (semitoneShift < -24.0f) {
        semitoneShift = -24.0f;
    }
    if (semitoneShift > 24.0f) {
        semitoneShift = 24.0f;
    }

    if (mix < 0.0f) {
        mix = 0.0f;
    }
    if (mix > 1.0f) {
        mix = 1.0f;
    }

    if (outputGain < 0.0f) {
        outputGain = 0.0f;
    }
    if (outputGain > 4.0f) {
        outputGain = 4.0f;
    }

    const float rate = powf(2.0f, semitoneShift / 12.0f);
    const float frameCountF = (float)frames;

    for (uint32_t frame = 0; frame < frames; ++frame) {
        float readPos = (float)frame * rate;
        while (readPos >= frameCountF) {
            readPos -= frameCountF;
        }
        while (readPos < 0.0f) {
            readPos += frameCountF;
        }

        uint32_t i0 = (uint32_t)readPos;
        uint32_t i1 = (i0 + 1U) % frames;
        float frac = readPos - (float)i0;

        float shifted = (in[i0] * (1.0f - frac)) + (in[i1] * frac);
        shifted *= inputGain;

        float dry = in[frame] * inputGain;
        float s = ((dry * (1.0f - mix)) + (shifted * mix)) * outputGain;

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
