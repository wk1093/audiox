#include "audio/effects/reverb.hpp"

#include <array>
#include <cstdint>

namespace audiox::effects {

namespace {

constexpr uint32_t kCombCount = 4;
constexpr uint32_t kCombLengths[kCombCount] = {521, 733, 941, 1153};

} // namespace

void processReverb(const float *in,
                   float *out,
                   uint32_t frames,
                   float inputGain,
                   float feedback,
                   float damping,
                   float wetMix,
                   ReverbState *state) {
    if (!in || !out || !state) {
        return;
    }

    if (inputGain < 0.0f) {
        inputGain = 0.0f;
    }
    if (inputGain > 4.0f) {
        inputGain = 4.0f;
    }

    if (feedback < 0.05f) {
        feedback = 0.05f;
    }
    if (feedback > 0.95f) {
        feedback = 0.95f;
    }

    if (damping < 0.0f) {
        damping = 0.0f;
    }
    if (damping > 1.0f) {
        damping = 1.0f;
    }

    if (wetMix < 0.0f) {
        wetMix = 0.0f;
    }
    if (wetMix > 1.0f) {
        wetMix = 1.0f;
    }

    for (uint32_t frame = 0; frame < frames; ++frame) {
        float x = in[frame] * inputGain;

        float sum = 0.0f;
        for (uint32_t c = 0; c < kCombCount; ++c) {
            uint32_t idx = state->combIndex[c];
            float y = state->comb[c][idx];
            state->comb[c][idx] = x + (y * feedback);

            ++idx;
            if (idx >= kCombLengths[c]) {
                idx = 0;
            }
            state->combIndex[c] = idx;
            sum += y;
        }

        sum *= 0.25f;
        state->lowpass = (state->lowpass * damping) + (sum * (1.0f - damping));

        uint32_t apIdx = state->allpassIndex;
        float apBuf = state->allpass[apIdx];
        float apIn = state->lowpass;
        float apOut = -apIn + apBuf;
        state->allpass[apIdx] = apIn + (apBuf * 0.5f);

        ++apIdx;
        if (apIdx >= ReverbState::kAllpassLength) {
            apIdx = 0;
        }
        state->allpassIndex = apIdx;

        float s = (x * (1.0f - wetMix)) + (apOut * wetMix);
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
