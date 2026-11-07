#pragma once

#include <cstdint>
#include "audio/effects/slot.hpp"

namespace audiox::graph {

constexpr unsigned kGraphNodes = 64;
constexpr unsigned kGraphChannels = 16;
struct BlockNode {
    float **inputs{};
    float **outputs{};
    float *inputStorage[kGraphChannels]{};
    float *outputStorage[kGraphChannels]{};
    uint8_t *contributions{};
    uint16_t *silentChannels{};
    audiox::effects::SlotParams params{};
    void *states[kGraphChannels]{};
    const void *plan{};
    float gain{};
    uint8_t channels{}, outputChannels{};
    bool effect{}, plugin{}, blocked{};
};
struct Block {
    BlockNode nodes[kGraphNodes]{};
    float *silence{};
    uint64_t *processedChannels{};
    void *pluginContext{};
    void (*processPlugin)(void *, uint32_t){};
};
struct BlockRoute {
    uint32_t src{}, dst{}, srcChannel{}, dstChannel{};
};

} // namespace audiox::graph
