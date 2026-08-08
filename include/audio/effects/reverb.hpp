#pragma once

#include <cstdint>
#include <array>

namespace audiox::effects {

struct ReverbState {
    static constexpr uint32_t kCombCount = 4;
    static constexpr uint32_t kMaxCombLength = 1153;
    static constexpr uint32_t kAllpassLength = 307;

    std::array<std::array<float, kMaxCombLength>, kCombCount> comb{};
    std::array<uint32_t, kCombCount> combIndex{};
    std::array<float, kCombCount> combFilter{};
    std::array<float, kAllpassLength> allpass{};
    uint32_t allpassIndex = 0;
};

void processReverb(const float *in,
                   float *out,
                   uint32_t frames,
                   float inputGain,
                   float feedback,
                   float damping,
                   float wetMix,
                   ReverbState *state);

} // namespace audiox::effects
