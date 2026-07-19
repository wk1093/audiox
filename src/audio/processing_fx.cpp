#include "audio/processing_fx.hpp"

#include <cmath>

namespace audiox::processing {

float semitoneToPitchRatio(int semitones) {
    return powf(2.0f, (float)semitones / 12.0f);
}

float clampPitchRatio(float ratio) {
    if (!(ratio > 0.0f)) {
        return 1.0f;
    }
    if (ratio < 0.125f) {
        return 0.125f;
    }
    if (ratio > 8.0f) {
        return 8.0f;
    }
    return ratio;
}

float computePlaybackStep(uint32_t sourceRate, uint32_t targetRate, float pitchRatio) {
    if (sourceRate == 0U || targetRate == 0U) {
        return 1.0f;
    }
    const float sr = (float)sourceRate / (float)targetRate;
    return sr * clampPitchRatio(pitchRatio);
}

} // namespace audiox::processing
