#include "audio/graph_processor.hpp"

#include <cstring>

namespace audiox::graph {

namespace {

bool validRoute(const BlockRoute& route, const BlockNode* nodes, uint16_t nodeCount) {
    return route.srcNode < nodeCount && route.dstNode < nodeCount &&
        route.srcChannel < nodes[route.srcNode].outputChannels &&
        route.dstChannel < nodes[route.dstNode].inputChannels;
}

void routeBlock(const BlockRoute& route, BlockNode* nodes, const float* nodeGains, uint32_t frames) {
    BlockNode& source = nodes[route.srcNode];
    BlockNode& destination = nodes[route.dstNode];
    const float gain = nodeGains ? nodeGains[route.srcNode] : 1.0f;
    const float* sourceBlock = source.outputs[route.srcChannel];
    float*& destinationBlock = destination.inputs[route.dstChannel];
    float* destinationStorage = destination.inputStorage[route.dstChannel];
    uint8_t& contributionCount = destination.inputContributionCount[route.dstChannel];

    if (contributionCount == 0U) {
        if (gain == 1.0f) {
            destinationBlock = const_cast<float*>(sourceBlock);
        } else {
            for (uint32_t frame = 0; frame < frames; ++frame) {
                destinationStorage[frame] = sourceBlock[frame] * gain;
            }
            destinationBlock = destinationStorage;
        }
        contributionCount = 1U;
        return;
    }

    if (destinationBlock != destinationStorage) {
        std::memcpy(destinationStorage, destinationBlock, sizeof(float) * frames);
        destinationBlock = destinationStorage;
    }

    if (gain == 1.0f) {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            destinationStorage[frame] += sourceBlock[frame];
        }
    } else {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            destinationStorage[frame] += sourceBlock[frame] * gain;
        }
    }

    if (contributionCount < 255U) {
        ++contributionCount;
    }
}

void copyClamped(const float* source, float* destination, uint32_t frames) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        float sample = source[frame];
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        destination[frame] = sample;
    }
}

void processNode(BlockNode& node, uint32_t frames) {
    const uint8_t channels = node.inputChannels < node.outputChannels
        ? node.inputChannels : node.outputChannels;

    if (node.kind == BlockNodeKind::Effect && node.effectParams && node.effectParams->enabled) {
        for (uint8_t channel = 0; channel < channels; ++channel) {
            effects::processSlot(node.id,
                                 static_cast<uint8_t>(node.channelOffset + channel),
                                 *node.effectParams,
                                 node.inputs[channel],
                                 node.outputs[channel],
                                 frames);
        }
    } else {
        for (uint8_t channel = 0; channel < channels; ++channel) {
            copyClamped(node.inputs[channel], node.outputs[channel], frames);
        }
    }

    for (uint8_t channel = channels; channel < node.outputChannels; ++channel) {
        std::memset(node.outputs[channel], 0, sizeof(float) * frames);
    }
}

}  // namespace

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
                 const uint16_t* sourceNodes,
                 uint16_t sourceNodeCount,
                 const uint16_t* sinkNodes,
                 uint16_t sinkNodeCount,
                 const BlockDeviceAdapter* deviceAdapter) {
    if ((!nodes && nodeCount != 0) || (!processNodes && processNodeCount != 0) || frames == 0) {
        return -1;
    }

    if (deviceAdapter && deviceAdapter->render) {
        if (!sourceNodes && sourceNodeCount != 0) {
            return -1;
        }
        for (uint16_t index = 0; index < sourceNodeCount; ++index) {
            const uint16_t nodeIndex = sourceNodes[index];
            if (nodeIndex >= nodeCount || nodes[nodeIndex].kind != BlockNodeKind::Source ||
                deviceAdapter->render(deviceAdapter->context, nodeIndex, nodes[nodeIndex], frames) != 0) {
                return -1;
            }
        }
    }

    for (uint16_t index = 0; index < sourceRouteCount; ++index) {
        const BlockRoute& route = sourceRoutes[index];
        if (!validRoute(route, nodes, nodeCount)) {
            return -1;
        }
        routeBlock(route, nodes, nodeGains, frames);
    }

    for (uint16_t index = 0; index < processNodeCount; ++index) {
        const uint16_t nodeIndex = processNodes[index];
        if (nodeIndex >= nodeCount) {
            return -1;
        }
        processNode(nodes[nodeIndex], frames);

        const uint16_t start = processRouteStart ? processRouteStart[nodeIndex] : 0;
        const uint16_t length = processRouteLength ? processRouteLength[nodeIndex] : 0;
        for (uint16_t routeIndex = 0; routeIndex < length; ++routeIndex) {
            const BlockRoute& route = processRoutesBySrc[start + routeIndex];
            if (!validRoute(route, nodes, nodeCount)) {
                return -1;
            }
            routeBlock(route, nodes, nodeGains, frames);
        }
    }

    if (deviceAdapter && deviceAdapter->write) {
        if (!sinkNodes && sinkNodeCount != 0) {
            return -1;
        }
        for (uint16_t index = 0; index < sinkNodeCount; ++index) {
            const uint16_t nodeIndex = sinkNodes[index];
            if (nodeIndex >= nodeCount || nodes[nodeIndex].kind != BlockNodeKind::Sink ||
                deviceAdapter->write(deviceAdapter->context, nodeIndex, nodes[nodeIndex], frames) != 0) {
                return -1;
            }
        }
    }

    return 0;
}

}  // namespace audiox::graph
