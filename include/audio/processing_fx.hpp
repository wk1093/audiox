#pragma once

#include <cstdint>

namespace audiox::processing {

struct FrameProcessor {
    virtual ~FrameProcessor() = default;
    virtual void processMono(const float *in, float *out, uint32_t frames) = 0;
};

float semitoneToPitchRatio(int semitones);
float clampPitchRatio(float ratio);
float computePlaybackStep(uint32_t sourceRate, uint32_t targetRate, float pitchRatio);

} // namespace audiox::processing
