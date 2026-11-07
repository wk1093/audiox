#include "audio/graph_block.hpp"
#include "audio/effects/pending.hpp"
#include "defs.hpp"
#include <cstring>

#define DECLARE(n) extern "C" void audiox_dsp_##n(const float *, float *, uint32_t, const float *, void *, const void *)
DECLARE(0); DECLARE(1); DECLARE(2); DECLARE(3); DECLARE(4); DECLARE(5);
DECLARE(6); DECLARE(7); DECLARE(8); DECLARE(9); DECLARE(10);
#undef DECLARE

using namespace audiox::graph;
using namespace audiox::effects;
extern "C" void audiox_native_route(Block *block, uint32_t srcNode, uint32_t dstNode,
                                    uint32_t srcChannel, uint32_t dstChannel) {
    auto &source = block->nodes[srcNode];
    auto &target = block->nodes[dstNode];
    const float gain = source.gain;
    const float *src = source.outputs[srcChannel];
    if (gain == 0 || src == block->silence || target.blocked) return;
    float *storage = target.inputStorage[dstChannel];
    float *dst = target.inputs[dstChannel];
    auto &count = target.contributions[dstChannel];
    if (!count) {
        if (gain == 1) target.inputs[dstChannel] = const_cast<float *>(src);
        else {
            for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) storage[frame] = src[frame] * gain;
            target.inputs[dstChannel] = storage;
        }
        count = 1;
        return;
    }
    if (dst != storage) {
        memcpy(storage, dst, sizeof(float) * BUFFER_FRAMES);
        dst = storage;
        target.inputs[dstChannel] = storage;
    }
    if (gain == 1) {
        for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) dst[frame] += src[frame];
    } else {
        for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) dst[frame] += src[frame] * gain;
    }
    if (count < 255) ++count;
}

extern "C" void audiox_native_node(Block *block, uint32_t index) {
    auto &node = block->nodes[index];
    if (!node.channels || !node.outputChannels) return;
    if (node.blocked) {
        for (unsigned channel = 0; channel < node.outputChannels; ++channel)
            node.outputs[channel] = block->silence;
        return;
    }
    if (!node.effect || !node.params.enabled) {
        for (unsigned channel = 0; channel < node.channels; ++channel)
            node.outputs[channel] = node.inputs[channel];
    } else if (node.plugin) {
        block->processPlugin(block->pluginContext, index);
        return;
    } else {
        for (unsigned channel = 0; channel < node.channels; ++channel) {
            const uint16_t bit = 1U << channel;
            const float *input = node.inputs[channel];
            bool silent = input == block->silence;
            if (!silent) {
                silent = true;
                for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame)
                    if (input[frame] != 0) { silent = false; break; }
            }
            if (silent && ((*node.silentChannels & bit) ||
                           !builtinNeedsSilentProcessing(node.params, node.states[channel]))) {
                node.outputs[channel] = block->silence;
                *node.silentChannels |= bit;
                continue;
            }
            *node.silentChannels &= ~bit;
            float *out = node.outputStorage[channel];
            node.outputs[channel] = out;
            const float *p = node.params.values;
            void *state = node.states[channel];
#define DSP(n) case n: audiox_dsp_##n(input, out, BUFFER_FRAMES, p, state, node.plan); break
            switch (node.params.type) {
            DSP(0); DSP(1); DSP(2); DSP(3); DSP(4); DSP(5);
            DSP(6); DSP(7); DSP(8); DSP(9); DSP(10);
            default: memset(out, 0, sizeof(float) * BUFFER_FRAMES); break;
            }
#undef DSP
            ++*block->processedChannels;
        }
    }
    for (unsigned channel = node.channels; channel < node.outputChannels; ++channel)
        node.outputs[channel] = block->silence;
}
