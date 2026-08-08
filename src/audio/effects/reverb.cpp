#include "audio/effects/reverb.hpp"

#include <array>
#include <cstdint>

namespace audiox::effects {

namespace {

constexpr uint32_t kCombCount = ReverbState::kCombCount;
constexpr uint32_t kCombLengths[kCombCount] = {
    1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617,
};
constexpr float kCombNorm = 1.0f / 8.0f;

constexpr uint32_t kAllpassCount = ReverbState::kAllpassCount;
constexpr uint32_t kAllpassLengths[kAllpassCount] = {556, 441, 341, 225};
constexpr float kAllpassFeedback = 0.5f;

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
            float filtered = (state->combFilter[c] * damping) + (y * (1.0f - damping));
            state->combFilter[c] = filtered;
            state->comb[c][idx] = x + (filtered * feedback);

            ++idx;
            if (idx >= kCombLengths[c]) {
                idx = 0;
            }
            state->combIndex[c] = idx;
            sum += y;
        }

        float wet = sum * kCombNorm;
        for (uint32_t a = 0; a < kAllpassCount; ++a) {
            uint32_t apIdx = state->allpassIndex[a];
            float apBuf = state->allpass[a][apIdx];
            float apIn = wet;
            float apOut = apBuf - apIn;
            state->allpass[a][apIdx] = apIn + (apBuf * kAllpassFeedback);

            ++apIdx;
            if (apIdx >= kAllpassLengths[a]) {
                apIdx = 0;
            }
            state->allpassIndex[a] = apIdx;
            wet = apOut;
        }

        float s = (x * (1.0f - wetMix)) + (wet * wetMix);
        out[frame] = s;
    }
}

} // namespace audiox::effects
