#include "audio/context.hpp"

#include "config/context.hpp"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

namespace {

static void copyBounded(char *dst, size_t dstSize, const char *src) {
    if (!dst || dstSize == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t n = strnlen(src, dstSize - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static void trimSpace(char *text) {
    if (!text || !text[0]) {
        return;
    }

    char *start = text;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }
    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[len - 1] = '\0';
        --len;
    }
}

static void formatRouteParseError(char *errorOut,
                                  size_t errorOutSize,
                                  const char *prefix,
                                  const char *route) {
    if (!errorOut || errorOutSize == 0 || !prefix) {
        return;
    }

    if (!route) {
        snprintf(errorOut, errorOutSize, "%s", prefix);
        return;
    }

    int prefixLen = snprintf(errorOut, errorOutSize, "%s: ", prefix);
    if (prefixLen < 0 || (size_t)prefixLen >= errorOutSize) {
        return;
    }

    size_t offset = (size_t)prefixLen;
    size_t remaining = errorOutSize - offset;
    if (remaining <= 1) {
        return;
    }

    size_t routeLen = strnlen(route, remaining - 1);
    memcpy(errorOut + offset, route, routeLen);
    errorOut[offset + routeLen] = '\0';
}

static void appendThing(AudioGraphThingInfo *out,
                        size_t cap,
                        size_t *count,
                        const char *id,
                        const char *name,
                        uint32_t inputs,
                        uint32_t outputs) {
    if (!out || !count || !id || !name || *count >= cap) {
        return;
    }

    AudioGraphThingInfo &thing = out[*count];
    memset(&thing, 0, sizeof(thing));
    copyBounded(thing.id, sizeof(thing.id), id);
    copyBounded(thing.name, sizeof(thing.name), name);
    thing.inputs = (uint8_t)((inputs > 16U) ? 16U : inputs);
    thing.outputs = (uint8_t)((outputs > 16U) ? 16U : outputs);
    ++(*count);
}

static const AudioGraphThingInfo *findThing(const AudioGraphThingInfo *things,
                                            size_t count,
                                            const char *id) {
    if (!things || !id) {
        return nullptr;
    }

    for (size_t i = 0; i < count; ++i) {
        if (strcmp(things[i].id, id) == 0) {
            return &things[i];
        }
    }
    return nullptr;
}

static int parseRouteString(const char *route,
                            AudioGraphEdgeInfo *edgeOut,
                            char *errorOut,
                            size_t errorOutSize) {
    if (!route || !edgeOut) {
        return RET_ERR;
    }

    char buf[256];
    copyBounded(buf, sizeof(buf), route);

    char *fields[4] = {};
    size_t fieldCount = 0;
    char *cursor = buf;
    while (fieldCount < 4) {
        fields[fieldCount++] = cursor;
        char *comma = strchr(cursor, ',');
        if (!comma) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }

    if (fieldCount != 4 || strchr(fields[3], ',') != nullptr) {
        if (errorOut && errorOutSize > 0) {
            formatRouteParseError(errorOut, errorOutSize, "bad route format", route);
        }
        return RET_ERR;
    }

    for (size_t i = 0; i < 4; ++i) {
        trimSpace(fields[i]);
    }
    if (!fields[0][0] || !fields[1][0]) {
        if (errorOut && errorOutSize > 0) {
            formatRouteParseError(errorOut, errorOutSize, "missing route endpoint", route);
        }
        return RET_ERR;
    }

    char *endp = nullptr;
    long srcChannel = strtol(fields[2], &endp, 10);
    if (!endp || *endp != '\0' || srcChannel < 0 || srcChannel > 255) {
        if (errorOut && errorOutSize > 0) {
            formatRouteParseError(errorOut, errorOutSize, "invalid source channel", route);
        }
        return RET_ERR;
    }

    long dstChannel = strtol(fields[3], &endp, 10);
    if (!endp || *endp != '\0' || dstChannel < 0 || dstChannel > 255) {
        if (errorOut && errorOutSize > 0) {
            formatRouteParseError(errorOut, errorOutSize, "invalid destination channel", route);
        }
        return RET_ERR;
    }

    memset(edgeOut, 0, sizeof(*edgeOut));
    copyBounded(edgeOut->src, sizeof(edgeOut->src), fields[0]);
    copyBounded(edgeOut->dst, sizeof(edgeOut->dst), fields[1]);
    edgeOut->srcChannel = (uint8_t)srcChannel;
    edgeOut->dstChannel = (uint8_t)dstChannel;
    return RET_OK;
}

static void formatGraphStatus(char *out,
                              size_t outSize,
                              uint16_t thingCount,
                              uint16_t edgeCount,
                              uint16_t skippedEdgeCount) {
    if (!out || outSize == 0) {
        return;
    }

    if (skippedEdgeCount > 0) {
        snprintf(out,
                 outSize,
                 "graph loaded with %u thing(s), %u active edge(s), %u skipped edge(s)",
                 (unsigned)thingCount,
                 (unsigned)edgeCount,
                 (unsigned)skippedEdgeCount);
    } else {
        snprintf(out,
                 outSize,
                 "graph loaded with %u thing(s) and %u edge(s)",
                 (unsigned)thingCount,
                 (unsigned)edgeCount);
    }
}

static void publishRoutingGraph(AudioContext *ctx, const AudioGraphState &graph) {
    if (!ctx) {
        return;
    }

    uint32_t seq = ctx->routingGraphSeq.load(std::memory_order_relaxed);
    ctx->routingGraphSeq.store(seq + 1, std::memory_order_release);
    ctx->routingGraphPublished = graph;
    ctx->routingGraphSeq.store(seq + 2, std::memory_order_release);
}

} // namespace

size_t AudioContext::copyRoutingThings(AudioGraphThingInfo *out, size_t cap) const {
    if (!out || cap == 0) {
        return 0;
    }

    ConfigData cfg = {};
    if (app && app->config) {
        cfg = app->config->readConfigFile();
    }

    AudioDeviceInfo infos[32];
    size_t deviceCount = copyDeviceInfos(infos, sizeof(infos) / sizeof(infos[0]));
    for (size_t i = 1; i < deviceCount; ++i) {
        AudioDeviceInfo key = infos[i];
        size_t j = i;
        while (j > 0) {
            const AudioDeviceInfo &prev = infos[j - 1];
            if (prev.cardIndex < key.cardIndex ||
                (prev.cardIndex == key.cardIndex && prev.deviceIndex <= key.deviceIndex)) {
                break;
            }
            infos[j] = infos[j - 1];
            --j;
        }
        infos[j] = key;
    }

    int haveGadgetPlayback = 0;
    int haveGadgetCapture = 0;
    for (size_t i = 0; i < deviceCount; ++i) {
        if (!infos[i].isGadget) {
            continue;
        }
        if (infos[i].hasPlayback) {
            haveGadgetPlayback = 1;
        }
        if (infos[i].hasCapture) {
            haveGadgetCapture = 1;
        }
    }

    size_t count = 0;
    appendThing(out, cap, &count, "soundboard_out", "Soundboard Out", 0, 2);
    appendThing(out, cap, &count, "fx_slot_0", "FX Slot 0", 2, 2);
    appendThing(out, cap, &count, "soundboard_virtual_out", "Soundboard Virtual Out", 2, 0);
    if (!haveGadgetPlayback) {
        appendThing(out, cap, &count, "usb_gadget_out", "USB Gadget Out", cfg.playbackChannels, 0);
    }
    if (!haveGadgetCapture) {
        appendThing(out, cap, &count, "usb_gadget_in", "USB Gadget In", 0, cfg.captureChannels);
    }

    for (size_t i = 0; i < deviceCount; ++i) {
        char id[64];
        char label[160];
        if (infos[i].hasCapture) {
            snprintf(id,
                     sizeof(id),
                     "alsa_card%u_dev%u_in",
                     (unsigned)infos[i].cardIndex,
                     (unsigned)infos[i].deviceIndex);
            snprintf(label, sizeof(label), "%s In", infos[i].displayName);
            appendThing(out,
                        cap,
                        &count,
                        id,
                        label,
                        0,
                        infos[i].captureChannels ? infos[i].captureChannels : 2);
        }
        if (infos[i].hasPlayback) {
            snprintf(id,
                     sizeof(id),
                     "alsa_card%u_dev%u_out",
                     (unsigned)infos[i].cardIndex,
                     (unsigned)infos[i].deviceIndex);
            snprintf(label, sizeof(label), "%s Out", infos[i].displayName);
            appendThing(out,
                        cap,
                        &count,
                        id,
                        label,
                        infos[i].playbackChannels ? infos[i].playbackChannels : 2,
                        0);
        }
    }

    return count;
}

int AudioContext::reloadRoutingGraph() {
    AudioGraphState next = {};
    next.topologyGeneration = deviceGeneration;
    next.thingCount = (uint16_t)copyRoutingThings(next.things,
                                                  sizeof(next.things) / sizeof(next.things[0]));

    RouterConfig router = app && app->config ? app->config->router() : RouterConfig();
    int routeCount = router.getRouteCount();
    if (routeCount < 0) {
        snprintf(next.status, sizeof(next.status), "failed to read routing file");
        return RET_ERR;
    }

    int skipped = 0;
    char route[256];
    char parseError[160];
    for (int i = 0; i < routeCount; ++i) {
        router.getRoute(i, route, sizeof(route));
        if (!route[0]) {
            continue;
        }

        AudioGraphEdgeInfo edge = {};
        if (parseRouteString(route, &edge, parseError, sizeof(parseError)) != RET_OK) {
            ++skipped;
            copyBounded(next.status, sizeof(next.status), parseError);
            continue;
        }

        const AudioGraphThingInfo *src = findThing(next.things, next.thingCount, edge.src);
        const AudioGraphThingInfo *dst = findThing(next.things, next.thingCount, edge.dst);
        if (!src || !dst) {
            ++skipped;
            snprintf(next.status, sizeof(next.status), "skipped unavailable route %s -> %s", edge.src, edge.dst);
            continue;
        }
        if (edge.srcChannel >= src->outputs || edge.dstChannel >= dst->inputs) {
            ++skipped;
            snprintf(next.status,
                     sizeof(next.status),
                     "skipped out-of-range route %s[%u] -> %s[%u]",
                     edge.src,
                     (unsigned)edge.srcChannel,
                     edge.dst,
                     (unsigned)edge.dstChannel);
            continue;
        }
        if (next.edgeCount >= AUDIO_GRAPH_MAX_EDGES) {
            ++skipped;
            copyBounded(next.status, sizeof(next.status), "graph edge capacity reached");
            continue;
        }

        next.edges[next.edgeCount++] = edge;
    }

    next.skippedEdgeCount = (uint16_t)skipped;

    if (routeCount > 0 && next.edgeCount == 0 && skipped > 0) {
        snprintf(next.status,
                 sizeof(next.status),
                 "routing update rejected: no valid edges (%d invalid)",
                 skipped);
        printf("[AUDIO] [WARN] %s\n", next.status);
        return RET_WARN;
    }

    {
        std::lock_guard<std::mutex> lock(routingGraphMutex);
        next.generation = routingGraph.generation + 1;
        if (next.status[0] == '\0') {
            formatGraphStatus(next.status,
                              sizeof(next.status),
                              next.thingCount,
                              next.edgeCount,
                              next.skippedEdgeCount);
        }
        routingGraph = next;
        publishRoutingGraph(this, routingGraph);
    }

    printf("[AUDIO] [INFO] %s\n", next.status);
    return skipped > 0 ? RET_WARN : RET_OK;
}

int AudioContext::buildRoutingGraphJson(char *out, size_t outSize) const {
    if (!out || outSize == 0) {
        return RET_ERR;
    }

    std::lock_guard<std::mutex> lock(routingGraphMutex);
    int n = snprintf(out,
                     outSize,
                     "{\"ok\":true,\"generation\":%u,\"topologyGeneration\":%u,\"things\":%u,\"edges\":%u,\"skippedEdges\":%u,\"status\":\"%s\"}\n",
                     (unsigned)routingGraph.generation,
                     (unsigned)routingGraph.topologyGeneration,
                     (unsigned)routingGraph.thingCount,
                     (unsigned)routingGraph.edgeCount,
                     (unsigned)routingGraph.skippedEdgeCount,
                     routingGraph.status);
    if (n < 0 || (size_t)n >= outSize) {
        return RET_ERR;
    }
    return RET_OK;
}