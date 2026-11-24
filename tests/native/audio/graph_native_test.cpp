#include "audio/graph_native.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>
#include <memory>
#include <cmath>
#include "audio/effects/pitch.hpp"
#include "audio/effects/reverb.hpp"
#include "audio/effects/gate.hpp"
#include "audio/effects/dynamics.hpp"
#include "audio/effects/equalizer.hpp"
#include "audio/effects/noise_reduction.hpp"
#include "audio/effects/advanced_eq.hpp"
#include "audio/effects/bass_enhancer.hpp"
#include "audio/effects/stereo.hpp"
#include "audio/effects/slot.hpp"

#define DECLARE(n) extern "C" void audiox_dsp_##n(const float *, float *, uint32_t, const float *, void *, const void *)
DECLARE(0); DECLARE(1); DECLARE(2); DECLARE(3); DECLARE(4); DECLARE(5);
DECLARE(6); DECLARE(7); DECLARE(8); DECLARE(9); DECLARE(10);
DECLARE(11); DECLARE(12); DECLARE(13); DECLARE(14);
#undef DECLARE

using namespace audiox::graph;
static void route(void *opaque, uint32_t index) {
    static_cast<std::vector<unsigned> *>(opaque)->push_back(index);
}
static void node(void *opaque, uint32_t index) {
    static_cast<std::vector<unsigned> *>(opaque)->push_back(1000 + index);
}
static void waitFor(NativeGraph &graph, const Schedule &schedule) {
    std::vector<unsigned> actual;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!graph.execute(schedule.generation, &actual, route, node)) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<unsigned> expected;
    for (unsigned i = 0; i < schedule.count; ++i)
        expected.push_back(schedule.operations[i].index +
            (schedule.operations[i].kind == OperationKind::Node ? 1000 : 0));
    assert(actual == expected);
}
static void testDirectGraph(NativeGraph &graph) {
        using namespace audiox::effects;
        struct Fixture {
            Block block{};
            float silence[128]{};
            float source[2][128]{};
            float storage[5][128]{};
            float *inputs[5][1]{}, *outputs[5][1]{};
            uint8_t contributions[5]{};
            uint16_t silent[5]{};
            uint64_t processed{};
            Fixture() {
                block.silence = silence;
                block.processedChannels = &processed;
                for (unsigned i = 0; i < 5; ++i) {
                    auto &node = block.nodes[i];
                    node.inputs = inputs[i];
                    node.outputs = outputs[i];
                    node.inputStorage[0] = storage[i];
                    node.outputStorage[0] = storage[i];
                    node.contributions = &contributions[i];
                    node.silentChannels = &silent[i];
                    node.gain = 1;
                    node.channels = node.outputChannels = 1;
                }
                for (unsigned i : {2U, 3U}) {
                    block.nodes[i].effect = true;
                    block.nodes[i].params.enabled = 1;
                }
                block.nodes[2].params.type = EFFECT_GAIN;
                block.nodes[3].params.type = EFFECT_DISTORTION;
            }
            void clear() {
                for (unsigned i = 0; i < 5; ++i) {
                    inputs[i][0] = silence;
                    outputs[i][0] = silence;
                    contributions[i] = 0;
                }
                outputs[0][0] = source[0];
                outputs[1][0] = source[1];
            }
        };
        auto fixture = std::make_unique<Fixture>();
        Schedule schedule{};
        schedule.generation = 200;
        schedule.direct = 1;
        schedule.count = 6;
        schedule.routes[0] = {0, 2, 0, 0};
        schedule.routes[1] = {1, 2, 0, 0};
        schedule.routes[128] = {2, 3, 0, 0};
        schedule.routes[129] = {3, 4, 0, 0};
        schedule.operations[0] = {OperationKind::Route, 0};
        schedule.operations[1] = {OperationKind::Route, 1};
        schedule.operations[2] = {OperationKind::Node, 2};
        schedule.operations[3] = {OperationKind::Route, 128};
        schedule.operations[4] = {OperationKind::Node, 3};
        schedule.operations[5] = {OperationKind::Route, 129};
        graph.request(schedule);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!graph.ready(schedule.generation)) {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        const auto forbidden = [](void *, uint32_t) { assert(false && "built-in graph called back to main"); };
        for (unsigned setting = 0; setting < 24; ++setting) {
            fixture->clear();
            auto &gain = fixture->block.nodes[2].params;
            auto &distortion = fixture->block.nodes[3].params;
            gain.values[0] = .25f + setting % 4;
            gain.enabled = setting % 2;
            distortion.values[0] = 1;
            distortion.values[1] = 4;
            distortion.values[2] = .35f;
            distortion.values[3] = 1;
            distortion.enabled = setting % 3;
            float mixed[128], gained[128], expected[128];
            for (unsigned frame = 0; frame < 128; ++frame) {
                fixture->source[0][frame] = frame % 3 ? .1f : -.2f;
                fixture->source[1][frame] = frame % 5 ? .25f : -.4f;
                mixed[frame] = fixture->source[0][frame] + fixture->source[1][frame];
            }
            if (gain.enabled) audiox_dsp_0(mixed, gained, 128, gain.values, nullptr, nullptr);
            else std::copy_n(mixed, 128, gained);
            if (distortion.enabled) audiox_dsp_1(gained, expected, 128, distortion.values, nullptr, nullptr);
            else std::copy_n(gained, 128, expected);
            assert(graph.execute(schedule.generation, &fixture->block, forbidden, forbidden));
            for (unsigned frame = 0; frame < 128; ++frame)
                assert(std::fabs(fixture->inputs[4][0][frame] - expected[frame]) < 1.0e-5f);
        }
        puts("Direct compiled fan-in -> gain -> distortion graph matches reference without routing or DSP callbacks");
    }
static void testDsp(NativeGraph &graph, Schedule &schedule) {
        using namespace audiox::effects;
        struct States {
            PitchState pitch{};
            ReverbState reverb{};
            GateState gate{};
            CompressorState compressor{};
            LimiterState limiter{};
            EqualizerState equalizer{};
            MultibandCompressorState multiband{};
            NoiseReductionState noise{};
            AdvancedEqState advancedEq{};
            BassEnhancerState bass{};
            CrossfeedState crossfeed{};
            MaximizerState maximizer{};
            void *get(unsigned type) {
                switch (type) {
                case 2:
                    return &pitch;
                case 3:
                    return &reverb;
                case 4:
                    return &gate;
                case 6:
                    return &compressor;
                case 7:
                    return &limiter;
                case 8:
                    return &equalizer;
                case 9:
                    return &multiband;
                case 10:
                    return &noise;
                case 11:
                    return &advancedEq;
                case 12:
                    return &bass;
                case 13:
                    return &crossfeed;
                case 14:
                    return &maximizer;
                default:
                    return nullptr;
                }
            }
        };
        const DspEntry reference[]{audiox_dsp_0, audiox_dsp_1, audiox_dsp_2, audiox_dsp_3,
                                   audiox_dsp_4, audiox_dsp_5, audiox_dsp_6, audiox_dsp_7,
                                   audiox_dsp_8, audiox_dsp_9, audiox_dsp_10, audiox_dsp_11,
                                   audiox_dsp_12, audiox_dsp_13, audiox_dsp_14};
        static_assert(sizeof(reference) / sizeof(reference[0]) == kBuiltinDspCount);
        const float parameters[kBuiltinDspCount][8]{{3},
                                      {1, 4, .35f, 1},
                                      {1, 5, 1, 1, .001f, 12000, 0},
                                      {1, .64f, .48f, .26f},
                                      {-40, 2, 160, 30},
                                      {},
                                      {-18, 4, 10, 120, 6, 0, 1},
                                      {-1, 80, 3, 1},
                                      {150, 3, 1000, -2, 6000, 4, 1, 1},
                                      {200, 2500, -18, -18, -18, 4, 120, 0},
                                      {12, -55, .1f, .7f, 1, 1},
                                      {},
                                      {6, 8.5f, 2, 100, 30, 1, 0, 1},
                                      {700, 4.5f, 1},
                                      {-12, -1, 25}};
        PitchPlan plan;
        for (unsigned type = 0; type < kBuiltinDspCount; ++type) {
            auto original = std::make_unique<States>();
            auto compiled = std::make_unique<States>();
            float params[EFFECT_PARAM_MAX]{};
            std::copy_n(parameters[type], 8, params);
            if (type == EFFECT_ADVANCED_EQ) {
                for (unsigned band = 0; band < kAdvancedEqMaxBands; ++band) {
                    float *values = params + kAdvancedEqFirstBandParam + band * kAdvancedEqParamsPerBand;
                    values[0] = AEQ_PEAK;
                    values[1] = 60.0f * float(band + 1);
                    values[3] = 1.41f;
                }
                params[0] = -3;
                params[1] = 2;
                params[4] = 6;      // band 1: +6 dB low peak
                params[6] = AEQ_HIGH_SHELF;
                params[8] = -4;
            }
            const bool stereo = isStereoLinkedType(uint8_t(type));
            for (unsigned block = 0; block < 160; ++block) {
                if (block == 80) {
                    ++schedule.generation;
                    graph.request(schedule);
                    waitFor(graph, schedule);
                }
                if (block == 40 && type == 2)
                    params[1] = -3;
                if (block == 40 && type == EFFECT_ADVANCED_EQ)
                    params[1] = 3;  // live band-count change crossfades
                float input[128], right[128], expected[256], actual[256];
                for (unsigned frame = 0; frame < 128; ++frame) {
                    input[frame] = block < 100 ? .35f * std::sin((block * 128 + frame) * .05f) : 0;
                    right[frame] = block < 100 ? .5f * std::sin((block * 128 + frame) * .013f) : 0;
                }
                if (block == 0)
                    input[0] = 2;
                StereoIo referenceIo{{input, right}, {expected, expected + 128}};
                StereoIo compiledIo{{input, right}, {actual, actual + 128}};
                const void *referencePlan = stereo ? static_cast<const void *>(&referenceIo) : &plan;
                const void *compiledPlan = stereo ? static_cast<const void *>(&compiledIo) : &plan;
                reference[type](input, expected, 128, params, original->get(type), referencePlan);
                const auto *table = graph.dsp(schedule.generation);
                assert(table);
                table->entries[type](input, actual, 128, params, compiled->get(type), compiledPlan);
                for (unsigned frame = 0; frame < (stereo ? 256U : 128U); ++frame) {
                    const float tolerance = 1.0e-4f * (1 + std::fabs(expected[frame]));
                    if (!(std::fabs(actual[frame] - expected[frame]) <= tolerance)) {
                        std::fprintf(stderr,
                                     "DSP mismatch type=%u block=%u frame=%u reference=%g native=%g\n", type,
                                     block, frame, expected[frame], actual[frame]);
                        assert(false);
                    }
                }
            }
        }
        puts("All 15 runtime-compiled DSP kernels match reference audio through live pitch changes, tails "
             "and recompilation");
}
int main(int argc, char **argv) {
    assert(argc == 3);
    NativeGraph graph(argv[1], argv[2]);
    Schedule schedule{};
    schedule.generation = 1;
    std::vector<unsigned> empty;
    assert(!graph.execute(1, &empty, route, node));
    graph.request(schedule);
    waitFor(graph, schedule);
    testDsp(graph, schedule);
    for (unsigned generation = 2; generation <= 12; ++generation) {
        schedule.generation = generation;
        schedule.count = generation == 12 ? kMaxOperations : generation;
        for (unsigned i = 0; i < schedule.count; ++i)
            schedule.operations[i] = {i % 3 ? OperationKind::Route : OperationKind::Node,
                                     i % (i % 3 ? 256U : 64U)};
        assert(!graph.execute(generation, &empty, route, node));
        graph.request(schedule);
        waitFor(graph, schedule);
    }
    // Requests can coalesce; execution must never use a different generation.
    for (unsigned generation = 13; generation < 100; ++generation) {
        schedule.generation = generation;
        graph.request(schedule);
    }
    waitFor(graph, schedule);
    {
        NativeGraph unavailable("/nonexistent/audiox-graph-compiler");
        unavailable.request(schedule);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(!unavailable.execute(schedule.generation, &empty, route, node));
    }
    {
        NativeGraph rejected("/bin/false");
        rejected.request(schedule);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(!rejected.execute(schedule.generation, &empty, route, node));
    }
    testDirectGraph(graph);
    puts("LLVM native DSP graph: empty/full schedules, order, replacement, coalescing and compiler-failure handling passed");
}
