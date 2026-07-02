#ifndef AUDIO_ROUTER_MODEL_H
#define AUDIO_ROUTER_MODEL_H

#include <stdint.h>
#include <stddef.h>

#define AUDIO_MAX_CHANNELS 8
#define AUDIO_MAX_BUSES 32
#define AUDIO_MAX_NODES 32
#define AUDIO_MAX_EDGES 96

typedef enum audio_node_kind {
    AUDIO_NODE_SOURCE = 0,
    AUDIO_NODE_MIXER,
    AUDIO_NODE_EFFECT,
    AUDIO_NODE_OUTPUT
} audio_node_kind_t;

typedef struct audio_bus_desc {
    uint16_t channels;
    uint16_t reserved;
} audio_bus_desc_t;

typedef struct audio_node_desc {
    audio_node_kind_t kind;
    uint16_t input_bus;
    uint16_t output_bus;
    uint16_t param_block;
    uint16_t flags;
} audio_node_desc_t;

typedef struct audio_edge_desc {
    uint16_t src_node;
    uint16_t dst_node;
    uint16_t src_bus;
    uint16_t dst_bus;
} audio_edge_desc_t;

typedef struct audio_router_model {
    uint32_t generation;
    uint16_t bus_count;
    uint16_t node_count;
    uint16_t edge_count;
    uint16_t exec_count;
    audio_bus_desc_t buses[AUDIO_MAX_BUSES];
    audio_node_desc_t nodes[AUDIO_MAX_NODES];
    audio_edge_desc_t edges[AUDIO_MAX_EDGES];
    uint16_t exec_order[AUDIO_MAX_NODES];
} audio_router_model_t;

#endif
