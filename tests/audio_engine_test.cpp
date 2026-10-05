#include "audio/engine.hpp"
#include "audio/graph_processor.hpp"
#include "audio/pcm_convert.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

}  // namespace

int main() {
    const float quietSample = 0.00001f;
    if (audiox::pcm::toS16(quietSample) != 0 || audiox::pcm::toS32(quietSample) == 0 ||
        audiox::pcm::toS32(std::numeric_limits<float>::quiet_NaN()) != 0 ||
        audiox::pcm::toS32(2.0f) != 2147483647) {
        std::fprintf(stderr, "PCM conversion did not preserve quiet samples or clamp safely\n");
        return 1;
    }

    float sourceBlock[BUFFER_FRAMES] = {};
    float secondSourceBlock[BUFFER_FRAMES] = {};
    float effectInputStorage[BUFFER_FRAMES] = {};
    float effectOutput[BUFFER_FRAMES] = {};
    float sinkInputStorage[BUFFER_FRAMES] = {};
    float sinkOutput[BUFFER_FRAMES] = {};
    for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
        sourceBlock[frame] = 0.1f;
        secondSourceBlock[frame] = 0.2f;
    }
    float* sourceOutputs[1] = {sourceBlock};
    float* secondSourceOutputs[1] = {secondSourceBlock};
    float* effectInputs[1] = {effectInputStorage};
    float* effectOutputs[1] = {effectOutput};
    float effectStorage[1][BUFFER_FRAMES] = {};
    float* sinkInputs[1] = {sinkInputStorage};
    float* sinkOutputs[1] = {sinkOutput};
    float sinkStorage[1][BUFFER_FRAMES] = {};
    uint8_t effectContributions[1] = {};
    uint8_t sinkContributions[1] = {};
    audiox::effects::SlotParams graphGain = {};
    graphGain.enabled = 1;
    audiox::effects::setSlotDefaultsForType(&graphGain, audiox::effects::EFFECT_GAIN);
    graphGain.enabled = 1;
    graphGain.values[0] = 3.0f;
    audiox::graph::BlockNode graphNodes[4] = {};
    graphNodes[0].kind = audiox::graph::BlockNodeKind::Source;
    graphNodes[0].outputChannels = 1;
    graphNodes[0].outputs = sourceOutputs;
    graphNodes[1].kind = audiox::graph::BlockNodeKind::Source;
    graphNodes[1].outputChannels = 1;
    graphNodes[1].outputs = secondSourceOutputs;
    graphNodes[2].id = "graph_gain";
    graphNodes[2].kind = audiox::graph::BlockNodeKind::Effect;
    graphNodes[2].inputChannels = 1;
    graphNodes[2].outputChannels = 1;
    graphNodes[2].inputs = effectInputs;
    graphNodes[2].outputs = effectOutputs;
    graphNodes[2].inputStorage = effectStorage;
    graphNodes[2].inputContributionCount = effectContributions;
    graphNodes[2].effectParams = &graphGain;
    graphNodes[3].kind = audiox::graph::BlockNodeKind::Sink;
    graphNodes[3].inputChannels = 1;
    graphNodes[3].inputs = sinkInputs;
    graphNodes[3].outputs = sinkOutputs;
    graphNodes[3].inputStorage = sinkStorage;
    graphNodes[3].inputContributionCount = sinkContributions;
    const audiox::graph::BlockRoute sourceRoutes[] = {{0, 2, 0, 0}, {1, 2, 0, 0}};
    const audiox::graph::BlockRoute processRoutes[] = {{2, 3, 0, 0}};
    const uint16_t processOrder[] = {2};
    const uint16_t routeStarts[] = {0, 0, 0, 0};
    const uint16_t routeLengths[] = {0, 0, 1, 0};
    const float graphNodeGains[] = {0.5f, 1.0f, 1.0f, 1.0f};
    if (audiox::graph::processBlock(graphNodes,
                                    4,
                                    sourceRoutes,
                                    2,
                                    processOrder,
                                    1,
                                    processRoutes,
                                    routeStarts,
                                    routeLengths,
                                    graphNodeGains,
                                    BUFFER_FRAMES) != 0 ||
        !near(sinkInputs[0][0], 0.75f) || !near(sinkInputs[0][BUFFER_FRAMES - 1], 0.75f)) {
        std::fprintf(stderr, "shared graph block processor did not mix sources and process the graph\n");
        return 1;
    }

    audiox::AudioEngine engine(48000, 2);
    if (engine.addEffect("gain", audiox::effects::EFFECT_GAIN) != 0 ||
        engine.setEffectParameter("gain", "level", 2.0f) != 0) {
        std::fprintf(stderr, "could not configure gain effect\n");
        return 1;
    }

    const float input[] = {0.1f, -0.2f, 0.3f, -0.4f};
    float output[4] = {};
    if (engine.process(input, output, 2) != 0 ||
        !near(output[0], 0.2f) || !near(output[1], -0.4f) ||
        !near(output[2], 0.6f) || !near(output[3], -0.8f)) {
        std::fprintf(stderr, "effect chain did not process interleaved channels correctly\n");
        return 1;
    }

    if (engine.setEffectEnabled("gain", false) != 0 || engine.process(input, output, 2) != 0 ||
        !near(output[0], input[0]) || !near(output[1], input[1])) {
        std::fprintf(stderr, "bypass did not pass input unchanged\n");
        return 1;
    }

    if (engine.setEffectEnabled("gain", true) != 0 ||
        engine.setEffectParameter("gain", "unknown", 1.0f) == 0 ||
        engine.removeEffect("gain") != 0 || engine.effectCount() != 0) {
        std::fprintf(stderr, "effect control operations did not maintain chain state\n");
        return 1;
    }

    if (engine.addEffect("drive", audiox::effects::EFFECT_GAIN, false) != 0 ||
        engine.setEffectType("drive", audiox::effects::EFFECT_DISTORTION) != 0 ||
        engine.setEffectParameter("drive", "drive", 6.0f) != 0 ||
        engine.setEffectParameter("drive", "drive", std::numeric_limits<float>::quiet_NaN()) == 0) {
        std::fprintf(stderr, "effect type changes did not install the selected parameter set\n");
        return 1;
    }
    const auto effects = engine.effects();
    if (effects.size() != 1 || effects[0].type != audiox::effects::EFFECT_DISTORTION ||
        !near(effects[0].values[1], 6.0f)) {
        std::fprintf(stderr, "effect type or parameter value was not retained\n");
        return 1;
    }

    std::puts("audio engine block and live-control tests passed");
    return 0;
}
