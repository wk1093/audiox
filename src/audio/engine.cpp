#include "audio/engine.hpp"
#include "audio/graph_processor.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audiox {

namespace {

constexpr uint8_t kMaximumChannels = 16;
constexpr uint16_t kMaximumGraphNodes = static_cast<uint16_t>(AudioEngine::kMaximumEffects + 2);

struct InterleavedInputContext {
    const float* samples;
    float* output;
    uint32_t frameOffset;
    uint32_t channel;
    uint32_t channelCount;
};

int renderInterleavedInput(void* context,
                           uint16_t,
                           graph::BlockNode& node,
                           uint32_t frames) {
    const auto* input = static_cast<const InterleavedInputContext*>(context);
    if (!input || !input->samples || !node.outputs || !node.outputs[0]) {
        return -1;
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        node.outputs[0][frame] = input->samples[
            (static_cast<size_t>(input->frameOffset + frame) * input->channelCount) + input->channel];
    }
    return 0;
}

int writeInterleavedOutput(void* context,
                           uint16_t,
                           const graph::BlockNode& node,
                           uint32_t frames) {
    const auto* output = static_cast<const InterleavedInputContext*>(context);
    if (!output || !output->output || !node.inputs || !node.inputs[0]) {
        return -1;
    }
    for (uint32_t frame = 0; frame < frames; ++frame) {
        output->output[(static_cast<size_t>(output->frameOffset + frame) * output->channelCount) + output->channel] =
            node.inputs[0][frame];
    }
    return 0;
}

}  // namespace

AudioEngine::AudioEngine(uint32_t sampleRate, uint32_t channels)
    : sampleRate_(sampleRate), channels_(channels) {
}

uint32_t AudioEngine::sampleRate() const {
    return sampleRate_;
}

uint32_t AudioEngine::channels() const {
    return channels_;
}

size_t AudioEngine::effectCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return effects_.size();
}

int AudioEngine::addEffect(const std::string& effectId, uint8_t type, bool enabled) {
    if (effectId.empty() || type > effects::EFFECT_CUT) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto exists = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (exists != effects_.end()) {
        return -1;
    }
    if (effects_.size() >= kMaximumEffects) {
        return -1;
    }

    Effect effect = {};
    effect.id = effectId;
    effects::setSlotDefaultsForType(&effect.params, type);
    effect.params.type = type;
    effect.params.enabled = enabled ? 1U : 0U;
    effects_.push_back(effect);
    return 0;
}

int AudioEngine::removeEffect(const std::string& effectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (found == effects_.end()) {
        return -1;
    }
    effects_.erase(found);
    return 0;
}

int AudioEngine::setEffectType(const std::string& effectId, uint8_t type) {
    if (type > effects::EFFECT_CUT) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (found == effects_.end()) {
        return -1;
    }
    const uint8_t enabled = found->params.enabled;
    effects::setSlotDefaultsForType(&found->params, type);
    found->params.type = type;
    found->params.enabled = enabled;
    return 0;
}

int AudioEngine::setEffectEnabled(const std::string& effectId, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (found == effects_.end()) {
        return -1;
    }
    found->params.enabled = enabled ? 1U : 0U;
    return 0;
}

int AudioEngine::setEffectParameter(const std::string& effectId, const char* name, float value) {
    if (!std::isfinite(value)) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (found == effects_.end()) {
        return -1;
    }
    return effects::setSlotParamValue(&found->params, name, value);
}

std::vector<EngineEffectInfo> AudioEngine::effects() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EngineEffectInfo> result;
    result.reserve(effects_.size());
    for (const Effect& effect : effects_) {
        EngineEffectInfo info = {};
        info.id = effect.id;
        info.type = effect.params.type;
        info.enabled = effect.params.enabled;
        const effects::EffectTypeSpec* spec = effects::effectTypeSpecFor(effect.params.type);
        info.values.assign(effect.params.values, effect.params.values + spec->paramCount);
        result.push_back(info);
    }
    return result;
}

int AudioEngine::process(const float* input, float* output, uint32_t frames) {
    if (!input || !output || frames == 0 || channels_ == 0 || channels_ > kMaximumChannels) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(mutex_);

    float graphBuffers[kMaximumGraphNodes][2][BUFFER_FRAMES] = {};
    float* inputPointers[kMaximumGraphNodes] = {};
    float* outputPointers[kMaximumGraphNodes] = {};
    uint8_t contributions[kMaximumGraphNodes] = {};
    graph::BlockNode nodes[kMaximumGraphNodes] = {};
    graph::BlockRoute sourceRoutes[1] = {};
    graph::BlockRoute processRoutes[kMaximumEffects] = {};
    uint16_t processOrder[kMaximumEffects] = {};
    uint16_t routeStarts[kMaximumGraphNodes] = {};
    uint16_t routeLengths[kMaximumGraphNodes] = {};
    float nodeGains[kMaximumGraphNodes] = {};

    const uint16_t sinkNode = static_cast<uint16_t>(effects_.size() + 1U);
    const uint16_t nodeCount = static_cast<uint16_t>(sinkNode + 1U);
    nodes[0].id = "desktop_source";
    nodes[0].kind = graph::BlockNodeKind::Source;
    nodes[0].outputChannels = 1;
    nodes[0].outputs = &outputPointers[0];

    for (uint16_t index = 0; index < nodeCount; ++index) {
        inputPointers[index] = graphBuffers[index][0];
        outputPointers[index] = graphBuffers[index][1];
        nodes[index].inputs = &inputPointers[index];
        nodes[index].outputs = &outputPointers[index];
        nodes[index].inputStorage = graphBuffers[index];
        nodes[index].inputContributionCount = &contributions[index];
        nodeGains[index] = 1.0f;
    }

    for (uint16_t index = 0; index < effects_.size(); ++index) {
        const uint16_t nodeIndex = static_cast<uint16_t>(index + 1U);
        nodes[nodeIndex].id = effects_[index].id.c_str();
        nodes[nodeIndex].kind = graph::BlockNodeKind::Effect;
        nodes[nodeIndex].inputChannels = 1;
        nodes[nodeIndex].outputChannels = 1;
        nodes[nodeIndex].effectParams = &effects_[index].params;
        processOrder[index] = nodeIndex;

        const uint16_t routeIndex = index;
        const uint16_t nextNode = static_cast<uint16_t>(nodeIndex + 1U);
        processRoutes[routeIndex] = {nodeIndex, nextNode, 0, 0};
        routeStarts[nodeIndex] = routeIndex;
        routeLengths[nodeIndex] = 1;
    }

    sourceRoutes[0] = {0, static_cast<uint16_t>(effects_.empty() ? sinkNode : 1U), 0, 0};
    nodes[sinkNode].kind = graph::BlockNodeKind::Sink;
    nodes[sinkNode].inputChannels = 1;

    for (uint32_t frameOffset = 0; frameOffset < frames; frameOffset += BUFFER_FRAMES) {
        const uint32_t blockFrames = std::min<uint32_t>(BUFFER_FRAMES, frames - frameOffset);
        for (uint8_t channel = 0; channel < channels_; ++channel) {
            std::memset(contributions, 0, sizeof(contributions));
            for (uint16_t node = 0; node < nodeCount; ++node) {
                inputPointers[node] = graphBuffers[node][0];
            }
            InterleavedInputContext inputContext = {input, output, frameOffset, channel, channels_};
            const graph::BlockDeviceAdapter deviceAdapter = {
                &inputContext, renderInterleavedInput, writeInterleavedOutput};
            const uint16_t sourceNode = 0;
            const uint16_t sinkNodeIndex = sinkNode;
            for (uint16_t index = 0; index < effects_.size(); ++index) {
                nodes[index + 1U].channelOffset = channel;
            }

            if (graph::processBlock(nodes,
                                    nodeCount,
                                    sourceRoutes,
                                    1,
                                    processOrder,
                                    static_cast<uint16_t>(effects_.size()),
                                    processRoutes,
                                    routeStarts,
                                    routeLengths,
                                    nodeGains,
                                    blockFrames,
                                    &sourceNode,
                                    1,
                                    &sinkNodeIndex,
                                    1,
                                    &deviceAdapter) != 0) {
                return -1;
            }
        }
    }
    return 0;
}

}  // namespace audiox
