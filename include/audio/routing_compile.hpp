#pragma once

#include "audio/context.hpp"

#include <cstring>

namespace audiox::graph {

// Keep direct routes first, then helper paths in saved incoming/outgoing order.
inline uint16_t expandRoutingHelper(AudioGraphState *graph) {
    AudioGraphEdgeInfo compiled[AUDIO_GRAPH_MAX_EDGES]{};
    uint16_t count = 0;
    uint16_t skipped = 0;
    auto append = [&](const AudioGraphEdgeInfo &edge) {
        if (count < AUDIO_GRAPH_MAX_EDGES) compiled[count++] = edge;
        else ++skipped;
    };
    for (uint16_t i = 0; i < graph->edgeCount; ++i) {
        const auto &edge = graph->edges[i];
        if (strcmp(edge.src, "routing_helper") != 0 && strcmp(edge.dst, "routing_helper") != 0) {
            append(edge);
        }
    }
    for (uint16_t i = 0; i < graph->edgeCount; ++i) {
        const auto &incoming = graph->edges[i];
        if (strcmp(incoming.dst, "routing_helper") != 0) continue;
        for (uint16_t j = 0; j < graph->edgeCount; ++j) {
            const auto &outgoing = graph->edges[j];
            if (strcmp(outgoing.src, "routing_helper") != 0 ||
                incoming.dstChannel != outgoing.srcChannel) continue;
            AudioGraphEdgeInfo edge{};
            memcpy(edge.src, incoming.src, sizeof(edge.src));
            memcpy(edge.dst, outgoing.dst, sizeof(edge.dst));
            edge.srcChannel = incoming.srcChannel;
            edge.dstChannel = outgoing.dstChannel;
            append(edge);
        }
    }
    memcpy(graph->edges, compiled, sizeof(compiled));
    graph->edgeCount = count;
    return skipped;
}

} // namespace audiox::graph
