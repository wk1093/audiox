#ifndef AUDIO_ROUTER_MODEL_H
#define AUDIO_ROUTER_MODEL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define AUDIO_ROUTE_MAX_NODES 16
#define AUDIO_ROUTE_MAX_EDGES 128
#define AUDIO_ROUTE_ID_MAX 32
#define AUDIO_ROUTE_LABEL_MAX 64
#define AUDIO_ROUTE_MAX_GADGET_CHANNELS 16
#define AUDIO_ROUTE_CAPTURE_SRC_USB_MIC 0x01u
#define AUDIO_ROUTE_CAPTURE_SRC_USB_GADGET 0x02u
#define AUDIO_ROUTE_NODE_FLAG_CAPTURE_SOURCE 0x0001u
#define AUDIO_ROUTE_NODE_FLAG_CAPTURE_GADGET 0x0002u
#define AUDIO_ROUTE_NODE_FLAG_SOUNDBOARD_SOURCE 0x0004u
#define AUDIO_ROUTE_NODE_FLAG_GADGET_OUTPUT_SINK 0x0008u
#define AUDIO_ROUTE_NODE_FLAG_SOUND_VIRTUAL_SINK 0x0010u

typedef enum audio_route_node_kind {
    AUDIO_ROUTE_NODE_INPUT = 0,
    AUDIO_ROUTE_NODE_OUTPUT = 1,
    AUDIO_ROUTE_NODE_VIRTUAL = 2,
    AUDIO_ROUTE_NODE_EFFECT = 3
} audio_route_node_kind_t;

typedef struct audio_route_node {
    char id[AUDIO_ROUTE_ID_MAX];
    char label[AUDIO_ROUTE_LABEL_MAX];
    audio_route_node_kind_t kind;
    uint16_t channels;
    uint16_t flags;
} audio_route_node_t;

typedef struct audio_route_edge {
    uint16_t src_node;
    uint16_t dst_node;
    uint16_t src_channel;
    uint16_t dst_channel;
    uint16_t gain_q15;
    uint16_t flags;
} audio_route_edge_t;

typedef struct audio_route_graph {
    uint32_t generation;
    uint16_t node_count;
    uint16_t edge_count;
    audio_route_node_t nodes[AUDIO_ROUTE_MAX_NODES];
    audio_route_edge_t edges[AUDIO_ROUTE_MAX_EDGES];
} audio_route_graph_t;

typedef struct audio_route_mix_config {
    uint8_t gadget_channels;
    uint8_t capture_source_mask;
    uint8_t mic_to_gadget[AUDIO_ROUTE_MAX_GADGET_CHANNELS];
    uint8_t soundboard_to_gadget[AUDIO_ROUTE_MAX_GADGET_CHANNELS];
    uint8_t soundboard_to_virtual;
} audio_route_mix_config_t;

static inline uint16_t audio_route_min_u16(uint16_t a, uint16_t b) {
    return (a < b) ? a : b;
}

static inline uint16_t audio_route_cap_channels(uint16_t channels) {
    if (channels == 0) {
        return 1;
    }
    if (channels > AUDIO_ROUTE_MAX_GADGET_CHANNELS) {
        return AUDIO_ROUTE_MAX_GADGET_CHANNELS;
    }
    return channels;
}

static inline void audio_route_graph_clear(audio_route_graph_t *graph) {
    if (!graph) {
        return;
    }
    memset(graph, 0, sizeof(*graph));
}

static inline int audio_route_node_add(audio_route_graph_t *graph,
                                       const char *id,
                                       const char *label,
                                       audio_route_node_kind_t kind,
                                       uint16_t channels) {
    if (!graph || !id || !id[0] || graph->node_count >= AUDIO_ROUTE_MAX_NODES) {
        return -1;
    }

    uint16_t idx = graph->node_count;
    audio_route_node_t *node = &graph->nodes[idx];
    memset(node, 0, sizeof(*node));

    size_t id_len = strnlen(id, sizeof(node->id) - 1);
    memcpy(node->id, id, id_len);
    node->id[id_len] = '\0';

    if (label && label[0]) {
        size_t label_len = strnlen(label, sizeof(node->label) - 1);
        memcpy(node->label, label, label_len);
        node->label[label_len] = '\0';
    } else {
        size_t copy_len = strnlen(node->id, sizeof(node->id) - 1);
        memcpy(node->label, node->id, copy_len);
        node->label[copy_len] = '\0';
    }

    node->kind = kind;
    node->channels = audio_route_cap_channels(channels);
    node->flags = 0;

    graph->node_count = (uint16_t)(graph->node_count + 1);
    return (int)idx;
}

static inline int audio_route_node_add_with_flags(audio_route_graph_t *graph,
                                                  const char *id,
                                                  const char *label,
                                                  audio_route_node_kind_t kind,
                                                  uint16_t channels,
                                                  uint16_t flags) {
    int idx = audio_route_node_add(graph, id, label, kind, channels);
    if (idx < 0) {
        return idx;
    }
    graph->nodes[idx].flags = flags;
    return idx;
}

static inline int audio_route_find_node(const audio_route_graph_t *graph, const char *id) {
    if (!graph || !id || !id[0]) {
        return -1;
    }

    for (uint16_t i = 0; i < graph->node_count; ++i) {
        if (strcmp(graph->nodes[i].id, id) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static inline int audio_route_edge_add(audio_route_graph_t *graph,
                                       int src_node,
                                       int dst_node,
                                       uint16_t src_channel,
                                       uint16_t dst_channel,
                                       uint16_t gain_q15) {
    if (!graph || src_node < 0 || dst_node < 0) {
        return -1;
    }
    if (graph->edge_count >= AUDIO_ROUTE_MAX_EDGES) {
        return -1;
    }
    if (src_node >= graph->node_count || dst_node >= graph->node_count) {
        return -1;
    }

    const audio_route_node_t *src = &graph->nodes[src_node];
    const audio_route_node_t *dst = &graph->nodes[dst_node];
    if (src_channel >= src->channels || dst_channel >= dst->channels) {
        return -1;
    }

    audio_route_edge_t *edge = &graph->edges[graph->edge_count++];
    edge->src_node = (uint16_t)src_node;
    edge->dst_node = (uint16_t)dst_node;
    edge->src_channel = src_channel;
    edge->dst_channel = dst_channel;
    edge->gain_q15 = gain_q15 ? gain_q15 : 32767u;
    edge->flags = 0;
    return 0;
}

static inline void audio_route_graph_default(audio_route_graph_t *graph,
                                             uint16_t usb_capture_channels,
                                             uint16_t usb_playback_channels) {
    if (!graph) {
        return;
    }

    audio_route_graph_clear(graph);

    int mic = audio_route_node_add_with_flags(graph,
                                              "usb_mic_in",
                                              "USB Mic In",
                                              AUDIO_ROUTE_NODE_INPUT,
                                              usb_capture_channels,
                                              AUDIO_ROUTE_NODE_FLAG_CAPTURE_SOURCE);
    int soundboard = audio_route_node_add_with_flags(graph,
                                                     "soundboard_out",
                                                     "Soundboard Out",
                                                     AUDIO_ROUTE_NODE_VIRTUAL,
                                                     2,
                                                     AUDIO_ROUTE_NODE_FLAG_SOUNDBOARD_SOURCE);
    int gadget_in = audio_route_node_add_with_flags(graph,
                                                    "usb_gadget_in",
                                                    "USB Gadget In",
                                                    AUDIO_ROUTE_NODE_INPUT,
                                                    usb_capture_channels,
                                                    AUDIO_ROUTE_NODE_FLAG_CAPTURE_SOURCE | AUDIO_ROUTE_NODE_FLAG_CAPTURE_GADGET);
    int gadget = audio_route_node_add_with_flags(graph,
                                                 "usb_gadget_out",
                                                 "USB Gadget Out",
                                                 AUDIO_ROUTE_NODE_OUTPUT,
                                                 usb_playback_channels,
                                                 AUDIO_ROUTE_NODE_FLAG_GADGET_OUTPUT_SINK);
    int virtual_out = audio_route_node_add_with_flags(graph,
                                                      "soundboard_virtual_out",
                                                      "Soundboard Virtual Out",
                                                      AUDIO_ROUTE_NODE_VIRTUAL,
                                                      2,
                                                      AUDIO_ROUTE_NODE_FLAG_SOUND_VIRTUAL_SINK);
    int fx0 = audio_route_node_add_with_flags(graph,
                                              "fx_slot_0",
                                              "FX Slot 0",
                                              AUDIO_ROUTE_NODE_EFFECT,
                                              2,
                                              0);

    if (mic >= 0 && gadget >= 0) {
        uint16_t channels = audio_route_min_u16(graph->nodes[mic].channels, graph->nodes[gadget].channels);
        for (uint16_t ch = 0; ch < channels; ++ch) {
            (void)audio_route_edge_add(graph, mic, gadget, ch, ch, 32767u);
        }
    }

    if (soundboard >= 0 && gadget >= 0) {
        uint16_t channels = audio_route_min_u16(graph->nodes[soundboard].channels, graph->nodes[gadget].channels);
        for (uint16_t ch = 0; ch < channels; ++ch) {
            (void)audio_route_edge_add(graph, soundboard, gadget, ch, ch, 32767u);
        }
    }

    if (gadget_in >= 0 && gadget >= 0) {
        uint16_t channels = audio_route_min_u16(graph->nodes[gadget_in].channels, graph->nodes[gadget].channels);
        for (uint16_t ch = 0; ch < channels; ++ch) {
            (void)audio_route_edge_add(graph, gadget_in, gadget, ch, ch, 32767u);
        }
    }

    if (soundboard >= 0 && fx0 >= 0) {
        uint16_t channels = audio_route_min_u16(graph->nodes[soundboard].channels, graph->nodes[fx0].channels);
        for (uint16_t ch = 0; ch < channels; ++ch) {
            (void)audio_route_edge_add(graph, soundboard, fx0, ch, ch, 32767u);
        }
    }

    if (fx0 >= 0 && gadget >= 0) {
        uint16_t channels = audio_route_min_u16(graph->nodes[fx0].channels, graph->nodes[gadget].channels);
        for (uint16_t ch = 0; ch < channels; ++ch) {
            (void)audio_route_edge_add(graph, fx0, gadget, ch, ch, 32767u);
        }
    }

    if (soundboard >= 0 && virtual_out >= 0) {
        uint16_t channels = audio_route_min_u16(graph->nodes[soundboard].channels, graph->nodes[virtual_out].channels);
        for (uint16_t ch = 0; ch < channels; ++ch) {
            (void)audio_route_edge_add(graph, soundboard, virtual_out, ch, ch, 32767u);
        }
    }

    if (fx0 >= 0 && virtual_out >= 0) {
        uint16_t channels = audio_route_min_u16(graph->nodes[fx0].channels, graph->nodes[virtual_out].channels);
        for (uint16_t ch = 0; ch < channels; ++ch) {
            (void)audio_route_edge_add(graph, fx0, virtual_out, ch, ch, 32767u);
        }
    }

    graph->generation = 1;
}

static inline void audio_route_mix_config_default(audio_route_mix_config_t *cfg, uint16_t gadget_channels) {
    if (!cfg) {
        return;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->gadget_channels = (uint8_t)audio_route_cap_channels(gadget_channels);
    cfg->capture_source_mask = (uint8_t)(AUDIO_ROUTE_CAPTURE_SRC_USB_MIC | AUDIO_ROUTE_CAPTURE_SRC_USB_GADGET);

    for (uint8_t ch = 0; ch < cfg->gadget_channels; ++ch) {
        cfg->mic_to_gadget[ch] = 1;
        cfg->soundboard_to_gadget[ch] = (ch < 2) ? 1 : 0;
    }

    cfg->soundboard_to_virtual = 1;
}

static inline int audio_route_compile_mix(const audio_route_graph_t *graph,
                                          audio_route_mix_config_t *out_cfg,
                                          uint16_t fallback_gadget_channels) {
    if (!graph || !out_cfg) {
        return -1;
    }

    int gadget_idx = -1;
    for (uint16_t i = 0; i < graph->node_count; ++i) {
        if ((graph->nodes[i].flags & AUDIO_ROUTE_NODE_FLAG_GADGET_OUTPUT_SINK) ||
            strcmp(graph->nodes[i].id, "usb_gadget_out") == 0) {
            gadget_idx = (int)i;
            break;
        }
    }

    uint16_t gadget_channels = (gadget_idx >= 0) ? graph->nodes[gadget_idx].channels : fallback_gadget_channels;

    audio_route_mix_config_default(out_cfg, gadget_channels);
    memset(out_cfg->mic_to_gadget, 0, sizeof(out_cfg->mic_to_gadget));
    memset(out_cfg->soundboard_to_gadget, 0, sizeof(out_cfg->soundboard_to_gadget));
    out_cfg->soundboard_to_virtual = 0;

    uint8_t carries_usb_mic[AUDIO_ROUTE_MAX_NODES];
    uint8_t carries_usb_gadget_in[AUDIO_ROUTE_MAX_NODES];
    uint8_t carries_soundboard[AUDIO_ROUTE_MAX_NODES];
    memset(carries_usb_mic, 0, sizeof(carries_usb_mic));
    memset(carries_usb_gadget_in, 0, sizeof(carries_usb_gadget_in));
    memset(carries_soundboard, 0, sizeof(carries_soundboard));

    for (uint16_t i = 0; i < graph->node_count; ++i) {
        const audio_route_node_t *node = &graph->nodes[i];
        if ((node->flags & AUDIO_ROUTE_NODE_FLAG_CAPTURE_SOURCE) || strcmp(node->id, "usb_mic_in") == 0) {
            carries_usb_mic[i] = 1;
        }
        if ((node->flags & AUDIO_ROUTE_NODE_FLAG_CAPTURE_GADGET) || strcmp(node->id, "usb_gadget_in") == 0) {
            carries_usb_gadget_in[i] = 1;
        }
        if ((node->flags & AUDIO_ROUTE_NODE_FLAG_SOUNDBOARD_SOURCE) || strcmp(node->id, "soundboard_out") == 0) {
            carries_soundboard[i] = 1;
        }
    }

    for (uint16_t iter = 0; iter < graph->node_count; ++iter) {
        int changed = 0;
        for (uint16_t i = 0; i < graph->edge_count; ++i) {
            const audio_route_edge_t *edge = &graph->edges[i];
            if (edge->src_node >= graph->node_count || edge->dst_node >= graph->node_count) {
                continue;
            }

            uint16_t src = edge->src_node;
            uint16_t dst = edge->dst_node;
            if (carries_usb_mic[src] && !carries_usb_mic[dst]) {
                carries_usb_mic[dst] = 1;
                changed = 1;
            }
            if (carries_usb_gadget_in[src] && !carries_usb_gadget_in[dst]) {
                carries_usb_gadget_in[dst] = 1;
                changed = 1;
            }
            if (carries_soundboard[src] && !carries_soundboard[dst]) {
                carries_soundboard[dst] = 1;
                changed = 1;
            }
        }
        if (!changed) {
            break;
        }
    }

    uint8_t mic_routed_to_gadget = 0;
    uint8_t gadget_routed_to_gadget = 0;
    for (uint16_t i = 0; i < graph->edge_count; ++i) {
        const audio_route_edge_t *edge = &graph->edges[i];
        if (edge->src_node >= graph->node_count || edge->dst_node >= graph->node_count) {
            continue;
        }

        const audio_route_node_t *dst = &graph->nodes[edge->dst_node];
        int dst_is_gadget_out = ((dst->flags & AUDIO_ROUTE_NODE_FLAG_GADGET_OUTPUT_SINK) ||
                                 strcmp(dst->id, "usb_gadget_out") == 0) ? 1 : 0;
        if (!dst_is_gadget_out || edge->dst_channel >= out_cfg->gadget_channels) {
            continue;
        }

        if (carries_usb_mic[edge->src_node]) {
            mic_routed_to_gadget = 1;
        }
        if (carries_usb_gadget_in[edge->src_node]) {
            gadget_routed_to_gadget = 1;
        }
    }

    out_cfg->capture_source_mask = 0;
    if (mic_routed_to_gadget) {
        out_cfg->capture_source_mask = AUDIO_ROUTE_CAPTURE_SRC_USB_MIC;
    } else if (gadget_routed_to_gadget) {
        out_cfg->capture_source_mask = AUDIO_ROUTE_CAPTURE_SRC_USB_GADGET;
    }

    for (uint16_t i = 0; i < graph->edge_count; ++i) {
        const audio_route_edge_t *edge = &graph->edges[i];
        if (edge->src_node >= graph->node_count || edge->dst_node >= graph->node_count) {
            continue;
        }

        const audio_route_node_t *dst = &graph->nodes[edge->dst_node];

        if ((((dst->flags & AUDIO_ROUTE_NODE_FLAG_GADGET_OUTPUT_SINK) ||
              strcmp(dst->id, "usb_gadget_out") == 0)) &&
            edge->dst_channel < out_cfg->gadget_channels) {
            if ((out_cfg->capture_source_mask & AUDIO_ROUTE_CAPTURE_SRC_USB_MIC) &&
                carries_usb_mic[edge->src_node]) {
                out_cfg->mic_to_gadget[edge->dst_channel] = 1;
            }
            if ((out_cfg->capture_source_mask & AUDIO_ROUTE_CAPTURE_SRC_USB_GADGET) &&
                carries_usb_gadget_in[edge->src_node]) {
                out_cfg->mic_to_gadget[edge->dst_channel] = 1;
            }
            if (carries_soundboard[edge->src_node]) {
                out_cfg->soundboard_to_gadget[edge->dst_channel] = 1;
            }
        }

        if ((((dst->flags & AUDIO_ROUTE_NODE_FLAG_SOUND_VIRTUAL_SINK) ||
              strcmp(dst->id, "soundboard_virtual_out") == 0)) &&
            carries_soundboard[edge->src_node]) {
            out_cfg->soundboard_to_virtual = 1;
        }
    }

    return 0;
}

#endif
