#pragma once

#include "audio/effects/slot.hpp"
#include "defs.hpp"

#include <cstdint>

namespace audiox::graph {

enum class BlockNodeKind : uint8_t {
    Source = 0,
    Effect = 1,
    Sink = 2,
    Pass = 3,
};

struct BlockRoute {
    uint16_t srcNode;
    uint16_t dstNode;
    uint8_t srcChannel;
    uint8_t dstChannel;
};

struct BlockNode {
    const char* id;
    BlockNodeKind kind;
    uint8_t inputChannels;
    uint8_t outputChannels;
    uint8_t channelOffset;
    float** inputs;
    float** outputs;
    float (*inputStorage)[BUFFER_FRAMES];
    uint8_t* inputContributionCount;
    const effects::SlotParams* effectParams;
};

struct BlockDeviceAdapter {
    void* context;
    int (*render)(void* context, uint16_t nodeIndex, BlockNode& node, uint32_t frames);
    int (*write)(void* context, uint16_t nodeIndex, const BlockNode& node, uint32_t frames);
};

int processBlock(BlockNode* nodes,
                 uint16_t nodeCount,
                 const BlockRoute* sourceRoutes,
                 uint16_t sourceRouteCount,
                 const uint16_t* processNodes,
                 uint16_t processNodeCount,
                 const BlockRoute* processRoutesBySrc,
                 const uint16_t* processRouteStart,
                 const uint16_t* processRouteLength,
                 const float* nodeGains,
                 uint32_t frames,
                 const uint16_t* sourceNodes = nullptr,
                 uint16_t sourceNodeCount = 0,
                 const uint16_t* sinkNodes = nullptr,
                 uint16_t sinkNodeCount = 0,
                 const BlockDeviceAdapter* deviceAdapter = nullptr);

}  // namespace audiox::graph
