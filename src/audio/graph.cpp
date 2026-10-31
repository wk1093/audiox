#include "audio/context.hpp"

#include "bluetooth/context.hpp"
#include "bluetooth/route_id.hpp"
#include "config/context.hpp"

#include <ctype.h>
#include <algorithm>
#include <cmath>
#include <stdio.h>
#include <string.h>

#include <string>
#include <vector>

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

// Converts an ALSA card-based ID (alsa_card{N}_dev{M}_{dir}) to its stable form
// (alsa_{stableCardId}_dev{M}_{dir}) using the currently enumerated device list.
// Returns true if the ID was converted, false if already stable, not ALSA, or device not found.
static bool toStableRouteId(AudioContext *ctx, const char *id, char *out, size_t outSize) {
    if (!ctx || !id || !out || outSize == 0) {
        return false;
    }
    unsigned card = 0;
    unsigned device = 0;
    char dir[8] = {};
    if (sscanf(id, "alsa_card%u_dev%u_%7s", &card, &device, dir) != 3) {
        copyBounded(out, outSize, id);
        return false;
    }
    AudioDeviceInfo infos[32];
    size_t deviceCount = ctx->copyDeviceInfos(infos, sizeof(infos) / sizeof(infos[0]));
    for (size_t i = 0; i < deviceCount; ++i) {
        if (infos[i].cardIndex != card || infos[i].deviceIndex != device) {
            continue;
        }
        if (infos[i].stableCardId[0]) {
            snprintf(out, outSize, "alsa_%s_dev%u_%s", infos[i].stableCardId, device, dir);
            return true;
        }
        break;
    }
    copyBounded(out, outSize, id);
    return false;
}

static bool normalizeRouteThingId(AudioContext *ctx,
                                 const AudioGraphThingInfo *things,
                                 size_t thingCount,
                                 const char *id,
                                 char *out,
                                 size_t outSize) {
    if (!ctx || !things || !id || !out || outSize == 0) {
        return false;
    }

    if (findThing(things, thingCount, id)) {
        copyBounded(out, outSize, id);
        return true;
    }

    char stableCardId[64] = {};
    unsigned stableDevice = 0;
    char stableDir[8] = {};
    if (strncmp(id, "alsa_", 5) == 0) {
        const char *devTag = nullptr;
        const char *scan = id + 5;
        while ((scan = strstr(scan, "_dev")) != nullptr) {
            if (isdigit((unsigned char)scan[4])) {
                devTag = scan;
                break;
            }
            scan += 1;
        }
        size_t idLength = devTag ? (size_t)(devTag - (id + 5)) : 0;
        if (devTag && idLength > 0 && idLength < sizeof(stableCardId) &&
            sscanf(devTag, "_dev%u_%7s", &stableDevice, stableDir) == 2) {
            memcpy(stableCardId, id + 5, idLength);
            stableCardId[idLength] = '\0';
        } else {
            devTag = nullptr;
        }
        if (devTag) {
            AudioDeviceInfo infos[32];
            size_t deviceCount = ctx->copyDeviceInfos(infos, sizeof(infos) / sizeof(infos[0]));
            const bool wantCapture = strcmp(stableDir, "in") == 0;
            const bool wantPlayback = strcmp(stableDir, "out") == 0;
            for (size_t i = 0; i < deviceCount; ++i) {
                const AudioDeviceInfo &info = infos[i];
                if ((strcmp(info.stableCardId, stableCardId) != 0 &&
                     strcmp(info.legacyCardId, stableCardId) != 0) ||
                    info.deviceIndex != stableDevice) {
                    continue;
                }
                if (wantCapture && !info.hasCapture) {
                    continue;
                }
                if (wantPlayback && !info.hasPlayback) {
                    continue;
                }
                snprintf(out, outSize, "alsa_%s_dev%u_%s",
                         info.stableCardId,
                         (unsigned)info.deviceIndex,
                         stableDir);
                return true;
            }
        }
    }

    unsigned card = 0;
    unsigned device = 0;
    char dir[8] = {};
    if (sscanf(id, "alsa_card%u_dev%u_%7s", &card, &device, dir) != 3) {
        copyBounded(out, outSize, id);
        return false;
    }

    const bool wantCapture = strcmp(dir, "in") == 0;
    const bool wantPlayback = strcmp(dir, "out") == 0;
    if (!wantCapture && !wantPlayback) {
        copyBounded(out, outSize, id);
        return false;
    }

    AudioDeviceInfo infos[32];
    size_t deviceCount = ctx->copyDeviceInfos(infos, sizeof(infos) / sizeof(infos[0]));
    for (size_t i = 0; i < deviceCount; ++i) {
        const AudioDeviceInfo &info = infos[i];
        if (info.cardIndex != card || info.deviceIndex != device) {
            continue;
        }
        if (wantCapture && !info.hasCapture) {
            continue;
        }
        if (wantPlayback && !info.hasPlayback) {
            continue;
        }

        const char *slug = info.stableCardId[0] ? info.stableCardId : nullptr;
        if (slug) {
            snprintf(out, outSize, "alsa_%s_dev%u_%s", slug, device, dir);
        } else {
            copyBounded(out, outSize, id);
        }
        return true;
    }

    copyBounded(out, outSize, id);
    return false;
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

static bool isLegacyBluetoothId(const char *id) {
    return id && (strcmp(id, "bluetooth_in") == 0 || strcmp(id, "bluetooth_out") == 0);
}

// Bluetooth endpoints that are not currently present (device forgotten or not
// yet re-paired). Routes to them are kept in routing.txt but stay inactive.
static bool isDormantBluetoothId(const AudioGraphThingInfo *things, size_t count, const char *id) {
    if (findThing(things, count, id)) {
        return false;
    }
    return bt::parseRouteId(id, nullptr) != 0 || isLegacyBluetoothId(id);
}

// Rewrites legacy single "bluetooth_in"/"bluetooth_out" routes to every paired
// device with that role. Legacy lines stay untouched while no such device exists.
static void migrateLegacyBluetoothRoutes(AudioContext *ctx,
                                         RouterConfig &router,
                                         const AudioGraphThingInfo *things,
                                         size_t thingCount) {
    if (!ctx || !ctx->app || !ctx->app->config) {
        return;
    }
    const int routeCount = router.getRouteCount();
    if (routeCount <= 0) {
        return;
    }

    static constexpr size_t kRouteLen = 256;
    std::vector<std::string> lines;
    lines.reserve((size_t)routeCount);
    bool migrated = false;

    for (int i = 0; i < routeCount; ++i) {
        char stored[kRouteLen] = {};
        router.getRoute(i, stored, sizeof(stored));
        if (!stored[0]) {
            continue;
        }
        AudioGraphEdgeInfo edge = {};
        const bool parsed = parseRouteString(stored, &edge, nullptr, 0) == RET_OK;
        const bool legacySrc = parsed && strcmp(edge.src, "bluetooth_in") == 0;
        const bool legacyDst = parsed && strcmp(edge.dst, "bluetooth_out") == 0;
        if (!legacySrc && !legacyDst) {
            lines.emplace_back(stored);
            continue;
        }

        const char *srcIds[BT_MAX_DEVICES + 1] = {edge.src};
        const char *dstIds[BT_MAX_DEVICES + 1] = {edge.dst};
        size_t srcCount = legacySrc ? 0 : 1;
        size_t dstCount = legacyDst ? 0 : 1;
        for (size_t t = 0; t < thingCount; ++t) {
            bool input = false;
            if (bt::parseRouteId(things[t].id, &input) == 0) {
                continue;
            }
            if (legacySrc && input && srcCount < BT_MAX_DEVICES) {
                srcIds[srcCount++] = things[t].id;
            }
            if (legacyDst && !input && dstCount < BT_MAX_DEVICES) {
                dstIds[dstCount++] = things[t].id;
            }
        }
        if (srcCount == 0 || dstCount == 0) {
            lines.emplace_back(stored);
            continue;
        }
        for (size_t s = 0; s < srcCount; ++s) {
            for (size_t d = 0; d < dstCount; ++d) {
                char line[kRouteLen];
                snprintf(line, sizeof(line), "%s,%s,%u,%u",
                         srcIds[s], dstIds[d],
                         (unsigned)edge.srcChannel, (unsigned)edge.dstChannel);
                lines.emplace_back(line);
            }
        }
        migrated = true;
    }

    if (!migrated) {
        return;
    }
    if (lines.size() > AUDIO_GRAPH_MAX_EDGES) {
        printf("[AUDIO] [WARN] legacy Bluetooth routes not migrated: %zu route(s) exceed the %u-route limit\n",
               lines.size(), (unsigned)AUDIO_GRAPH_MAX_EDGES);
        return;
    }
    std::vector<const char *> ptrs;
    ptrs.reserve(lines.size());
    for (const std::string &line : lines) {
        ptrs.push_back(line.c_str());
    }
    RouterConfig mutableRouter = ctx->app->config->router();
    mutableRouter.replaceAllRoutes(ptrs.data(), ptrs.size());
    printf("[AUDIO] [INFO] migrated legacy Bluetooth routes to per-device nodes (%zu route(s))\n",
           lines.size());
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
    RouterConfig router = app && app->config ? app->config->router() : RouterConfig();
    if (router.hasRoutingHelper()) {
        appendThing(out, cap, &count, "routing_helper", "Routing Helper", 2, 2);
    }
    AudioEffectSlotState fxStates[32] = {};
    int fxCount = listEffects(fxStates, sizeof(fxStates) / sizeof(fxStates[0]));
    if (fxCount < 0) {
        fxCount = 0;
    }
    for (int i = 0; i < fxCount; ++i) {
        if (!fxStates[i].thingId[0]) {
            continue;
        }
        char label[160];
        snprintf(label, sizeof(label), "FX %s", fxStates[i].thingId);
        uint32_t channels = 2;
        if (fxStates[i].type == audiox::effects::EFFECT_CUT) {
            for (uint8_t param = 0; param < fxStates[i].paramCount; ++param) {
                if (strcmp(fxStates[i].paramNames[param], "channels") == 0) {
                    channels = static_cast<uint32_t>(fxStates[i].paramValues[param] + 0.5f);
                    break;
                }
            }
            if (channels < 1) channels = 1;
            if (channels > 16) channels = 16;
        }
        appendThing(out, cap, &count, fxStates[i].thingId, label, channels, channels);
    }
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
            if (infos[i].stableCardId[0]) {
                snprintf(id, sizeof(id), "alsa_%s_dev%u_in",
                         infos[i].stableCardId, (unsigned)infos[i].deviceIndex);
            } else {
                snprintf(id, sizeof(id), "alsa_card%u_dev%u_in",
                         (unsigned)infos[i].cardIndex, (unsigned)infos[i].deviceIndex);
            }
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
            if (infos[i].stableCardId[0]) {
                snprintf(id, sizeof(id), "alsa_%s_dev%u_out",
                         infos[i].stableCardId, (unsigned)infos[i].deviceIndex);
            } else {
                snprintf(id, sizeof(id), "alsa_card%u_dev%u_out",
                         (unsigned)infos[i].cardIndex, (unsigned)infos[i].deviceIndex);
            }
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

    // Paired Bluetooth devices come last so they never crowd out wired devices.
    if (app && app->bluetooth) {
        BluetoothRoutingDevice btDevices[BT_MAX_DEVICES];
        size_t btCount = app->bluetooth->copyRoutingDevices(btDevices, BT_MAX_DEVICES);
        for (size_t i = 0; i < btCount; ++i) {
            char id[BT_ROUTE_ID_MAX];
            char display[BT_ADDRESS_TEXT + 16];
            char label[128];
            if (btDevices[i].name[0]) {
                snprintf(display, sizeof(display), "%.32s", btDevices[i].name);
            } else {
                char address[BT_ADDRESS_TEXT] = {};
                bt::formatAddress(btDevices[i].address, address, sizeof(address));
                snprintf(display, sizeof(display), "%s", address);
            }
            if (btDevices[i].input && bt::formatRouteId(btDevices[i].address.b, true, id, sizeof(id))) {
                snprintf(label, sizeof(label), "BT %s In", display);
                appendThing(out, cap, &count, id, label, 0, 2);
            }
            if (btDevices[i].output && bt::formatRouteId(btDevices[i].address.b, false, id, sizeof(id))) {
                snprintf(label, sizeof(label), "BT %s Out", display);
                appendThing(out, cap, &count, id, label, 2, 0);
            }
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
    migrateLegacyBluetoothRoutes(this, router, next.things, next.thingCount);
    int routeCount = router.getRouteCount();
    if (routeCount < 0) {
        snprintf(next.status, sizeof(next.status), "failed to read routing file");
        return RET_ERR;
    }

    int skipped = 0;
    int dormant = 0;
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

        char normalizedSrc[64];
        char normalizedDst[64];
        normalizeRouteThingId(this, next.things, next.thingCount, edge.src, normalizedSrc, sizeof(normalizedSrc));
        normalizeRouteThingId(this, next.things, next.thingCount, edge.dst, normalizedDst, sizeof(normalizedDst));
        copyBounded(edge.src, sizeof(edge.src), normalizedSrc);
        copyBounded(edge.dst, sizeof(edge.dst), normalizedDst);

        const AudioGraphThingInfo *src = findThing(next.things, next.thingCount, edge.src);
        const AudioGraphThingInfo *dst = findThing(next.things, next.thingCount, edge.dst);
        if (!src || !dst) {
            const bool srcOk = src || isDormantBluetoothId(next.things, next.thingCount, edge.src);
            const bool dstOk = dst || isDormantBluetoothId(next.things, next.thingCount, edge.dst);
            if (srcOk && dstOk) {
                ++dormant;
                continue;
            }
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

    if (dormant > 0) {
        printf("[AUDIO] [INFO] kept %d route(s) for Bluetooth devices that are not paired\n", dormant);
    }

    if (routeCount > 0 && next.edgeCount == 0 && skipped > 0) {
        snprintf(next.status,
                 sizeof(next.status),
                 "routing update rejected: no valid edges (%d invalid)",
                 skipped);
        printf("[AUDIO] [WARN] %s\n", next.status);
        return RET_WARN;
    }

    // Normalize any card-based device IDs in the stored routes to stable IDs.
    // This makes routing survive card re-enumeration on reboot or replug.
    if (routeCount > 0 && app && app->config) {
        static constexpr size_t kMaxRoutes = AUDIO_GRAPH_MAX_EDGES;
        static constexpr size_t kRouteLen = 256;
        char routeBufs[kMaxRoutes][kRouteLen];
        const char *routePtrs[kMaxRoutes];
        size_t normalizedCount = 0;
        bool dirty = false;

        for (int i = 0; i < routeCount && normalizedCount < kMaxRoutes; ++i) {
            char stored[kRouteLen] = {};
            router.getRoute(i, stored, sizeof(stored));
            if (!stored[0]) {
                continue;
            }

            AudioGraphEdgeInfo storedEdge = {};
            if (parseRouteString(stored, &storedEdge, nullptr, 0) != RET_OK) {
                snprintf(routeBufs[normalizedCount], kRouteLen, "%s", stored);
            } else {
                char stableSrc[64] = {};
                char stableDst[64] = {};
                bool srcChanged = toStableRouteId(this, storedEdge.src, stableSrc, sizeof(stableSrc));
                bool dstChanged = toStableRouteId(this, storedEdge.dst, stableDst, sizeof(stableDst));
                if (!srcChanged) {
                    normalizeRouteThingId(this, next.things, next.thingCount,
                                          storedEdge.src, stableSrc, sizeof(stableSrc));
                    srcChanged = strcmp(stableSrc, storedEdge.src) != 0;
                }
                if (!dstChanged) {
                    normalizeRouteThingId(this, next.things, next.thingCount,
                                          storedEdge.dst, stableDst, sizeof(stableDst));
                    dstChanged = strcmp(stableDst, storedEdge.dst) != 0;
                }
                if (srcChanged || dstChanged) {
                    dirty = true;
                    snprintf(routeBufs[normalizedCount], kRouteLen, "%s,%s,%u,%u",
                             stableSrc, stableDst,
                             (unsigned)storedEdge.srcChannel,
                             (unsigned)storedEdge.dstChannel);
                } else {
                    snprintf(routeBufs[normalizedCount], kRouteLen, "%s", stored);
                }
            }
            routePtrs[normalizedCount] = routeBufs[normalizedCount];
            ++normalizedCount;
        }

        if (dirty) {
            RouterConfig mutableRouter = app->config->router();
            mutableRouter.replaceAllRoutes(routePtrs, normalizedCount);
            printf("[AUDIO] [INFO] saved %zu route(s) with stable device identifiers\n", normalizedCount);
        }
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

int AudioContext::buildMetersJson(char *out, size_t outSize) const {
    if (!out || outSize == 0) {
        return RET_ERR;
    }

    std::string json = "{\"meters\":[";
    {
        std::lock_guard<std::mutex> lock(routingGraphMutex);
        for (uint16_t i = 0; i < routingGraphPublished.thingCount; ++i) {
            const AudioGraphThingInfo &thing = routingGraphPublished.things[i];
            json += i ? ",{\"id\":\"" : "{\"id\":\"";
            for (const unsigned char ch : std::string(thing.id)) {
                if (ch == '"' || ch == '\\' || ch < 0x20) {
                    char escaped[7];
                    snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned)ch);
                    json += escaped;
                } else {
                    json += static_cast<char>(ch);
                }
            }
            json += "\",\"levels\":[";
            const unsigned channels = std::min<unsigned>(thing.outputs ? thing.outputs : thing.inputs, 16);
            for (unsigned ch = 0; ch < channels; ++ch) {
                const float raw = nodeChannelLevels[i][ch].load(std::memory_order_relaxed);
                const float level = std::isfinite(raw) ? std::max(0.0f, std::min(raw, 1.0f)) : 0.0f;
                char value[24];
                snprintf(value, sizeof(value), "%s%.4f", ch ? "," : "", (double)level);
                json += value;
            }
            json += "]}";
        }
    }
    json += "]}\n";
    if (json.size() >= outSize) {
        return RET_ERR;
    }
    memcpy(out, json.c_str(), json.size() + 1);
    return RET_OK;
}