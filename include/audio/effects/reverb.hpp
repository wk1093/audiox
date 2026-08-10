#pragma once

#include <cstdint>
#include <array>

namespace audiox::effects {

struct ReverbState {
    static constexpr uint32_t kCombCount = 8;
    static constexpr uint32_t kMaxCombLength = 1617;
    static constexpr uint32_t kAllpassCount = 4;
    static constexpr uint32_t kMaxAllpassLength = 556;

    std::array<std::array<float, kMaxCombLength>, kCombCount> comb{};
    std::array<uint32_t, kCombCount> combIndex{};
    std::array<float, kCombCount> combFilter{};
    std::array<std::array<float, kMaxAllpassLength>, kAllpassCount> allpass{};
    std::array<uint32_t, kAllpassCount> allpassIndex{};
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
