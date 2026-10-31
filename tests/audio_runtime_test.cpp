#include "../src/audio/processing.cpp"
#include "audio/routing_compile.hpp"

#include <cassert>
#include <vector>

AudioContext::AudioContext(Audiox *context) : app(context) {}
AudioContext::~AudioContext() {}
int AudioContext::getEffectParams(const char *, audiox::effects::SlotParams *) const {
    return RET_ERR;
}

int main() {
    auto context = std::make_unique<AudioContext>(nullptr);
    auto runtime = std::make_unique<RuntimeGraph>();
    auto capture = std::make_unique<AlsaCaptureBackend>();
    capture->reset();
    runtime->holdVoiceIndex = -1;
    runtime->snapshot.thingCount = 2;
    strcpy(runtime->snapshot.things[0].id, "soundboard_out");
    runtime->snapshot.things[0].outputs = 2;
    strcpy(runtime->snapshot.things[1].id, "test_out");
    runtime->snapshot.things[1].inputs = 2;
    runtime->nodeKind[0] = NODE_SOURCE;
    runtime->nodeKind[1] = NODE_SINK;
    runtime->sourceNodes[0] = 0;
    runtime->sourceNodeCount = 1;
    runtime->sinkNodes[0] = 1;
    runtime->sinkNodeCount = 1;
    runtime->sourceRoutes[0] = {0, 1, 0, 0};
    runtime->sourceRoutes[1] = {0, 1, 1, 1};
    runtime->sourceRouteCount = 2;
    context->pendingSfxStopAll.store(0);
    context->pendingSfxHoldStops.store(0);
    context->sfxQueueRead.store(0);
    context->sfxQueueWrite.store(1);
    context->sfxQueue[0] = {0, 0, 1.0f};
    context->sfxSlotRefs[0].store(1);
    context->sfxIsPlaying.store(0);
    context->sfxPlayingSeq.store(0);
    context->sfxPlayingCount = 0;
    context->effectStatesSeq.store(0);
    context->nodeGainAtomics[0].store(1.0f);
    context->nodeGainAtomics[1].store(1.0f);
    int16_t samples[BUFFER_FRAMES * 2] = {};
    for (auto &sample : samples) sample = 16384;
    auto &clip = context->sfxSlots[0];
    clip = {};
    clip.loaded = 1;
    strcpy(clip.name, "test.wav");
    clip.pcm = samples;
    clip.frames = BUFFER_FRAMES * 2;
    clip.channels = 1;
    clip.sampleRate = SAMPLE_RATE;
    processGraphBlock(context.get(), runtime.get(), capture.get());
    assert(runtime->soundboardVoices[0].pos == BUFFER_FRAMES);
    assert(context->sfxIsPlaying.load() == 1);
    assert(context->sfxPlayingCount == 1);
    assert(runtime->inputs[1][0][0] > 0.1f);
    processGraphBlock(context.get(), runtime.get(), capture.get());
    assert(context->sfxIsPlaying.load() == 0);
    assert(context->sfxPlayingCount == 0);
    assert(context->sfxSlotRefs[0].load() == 0);
    puts("Runtime soundboard audio, completion and lighting tests passed");

    // Per-device Bluetooth nodes: only the live peer's node reads or writes audio.
    auto btRuntime = std::make_unique<RuntimeGraph>();
    btRuntime->snapshot.thingCount = 3;
    strcpy(btRuntime->snapshot.things[0].id, "bt_A4C6F0A57AA7_in");
    btRuntime->snapshot.things[0].outputs = 2;
    strcpy(btRuntime->snapshot.things[1].id, "bt_112233445566_in");
    btRuntime->snapshot.things[1].outputs = 2;
    strcpy(btRuntime->snapshot.things[2].id, "bt_112233445566_out");
    btRuntime->snapshot.things[2].inputs = 2;
    for (uint16_t node = 0; node < 3; ++node) {
        btRuntime->btAddress[node] = bt::parseRouteId(btRuntime->snapshot.things[node].id, nullptr);
        for (uint8_t ch = 0; ch < kMaxChannelsPerThing; ++ch) {
            btRuntime->outputs[node][ch] = btRuntime->outputStorage[node][ch];
            btRuntime->inputs[node][ch] = btRuntime->outputStorage[node][ch];
        }
    }
    assert(btRuntime->btAddress[0] == 0xA4C6F0A57AA7ULL);
    const uint8_t phoneAddress[6] = {0xA7, 0x7A, 0xA5, 0xF0, 0xC6, 0xA4};
    assert(bt::packAddress(phoneAddress) == btRuntime->btAddress[0]);
    char routeId[BT_ROUTE_ID_MAX];
    assert(bt::formatRouteId(phoneAddress, true, routeId, sizeof(routeId)));
    assert(strcmp(routeId, "bt_A4C6F0A57AA7_in") == 0);
    assert(bt::parseRouteId("bt_A4C6F0A57AA7_side", nullptr) == 0);
    assert(bt::parseRouteId("bluetooth_in", nullptr) == 0);

    // Two phones streaming at once, each into its own slot.
    BluetoothInputStream &phoneA = context->bluetoothInputs[0];
    BluetoothInputStream &phoneB = context->bluetoothInputs[2];
    phoneA.start(btRuntime->btAddress[0], 48000);
    phoneB.start(btRuntime->btAddress[1], 48000);
    const uint32_t target = phoneA.targetFill();
    std::vector<int16_t> btFrames((target + BUFFER_FRAMES * 8U) * 2U, 16384);
    std::vector<int16_t> btFramesB((target + BUFFER_FRAMES * 8U) * 2U, -8192);
    // Not primed yet: silence until the jitter buffer holds the target latency.
    btRuntime->outputStorage[0][0][0] = 1.0f;
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    assert(btRuntime->outputStorage[0][0][0] == 0.0f);
    assert(phoneA.push(btFrames.data(), target / 2U) == target / 2U);
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    assert(btRuntime->outputStorage[0][0][0] == 0.0f);
    assert(phoneA.read.load() == 0);
    assert(phoneA.push(btFrames.data(), target) == target);
    assert(phoneB.push(btFramesB.data(), target + BUFFER_FRAMES) == target + BUFFER_FRAMES);
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 1);
    assert(btRuntime->outputStorage[0][0][0] > 0.4f);
    assert(btRuntime->outputStorage[0][1][BUFFER_FRAMES - 1] > 0.4f);
    assert(btRuntime->outputStorage[1][0][0] < -0.2f);
    assert(phoneA.read.load() >= BUFFER_FRAMES - 2U);
    assert(phoneA.underruns.load() == 0);

    // Draining the buffer causes one underrun, a fade to silence, then rebuffering.
    for (int block = 0; block < 400 && phoneA.primed; ++block) {
        renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    }
    assert(!phoneA.primed);
    assert(phoneA.underruns.load() == 1);
    assert(phoneA.read.load() == phoneA.write.load());
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    assert(btRuntime->outputStorage[0][0][BUFFER_FRAMES - 1] == 0.0f);
    assert(phoneA.push(btFrames.data(), BUFFER_FRAMES) == BUFFER_FRAMES);
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    assert(btRuntime->outputStorage[0][0][0] == 0.0f);
    assert(phoneA.underruns.load() == 1);

    // 44.1 kHz input is read slower than real time to resample to 48 kHz.
    phoneA.start(btRuntime->btAddress[0], 44100);
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    const uint32_t start441 = phoneA.read.load();
    const uint32_t target441 = phoneA.targetFill();
    assert(target441 < target);
    assert(phoneA.push(btFrames.data(), target441 + BUFFER_FRAMES * 2U) == target441 + BUFFER_FRAMES * 2U);
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    const uint32_t consumed441 = phoneA.read.load() - start441;
    assert(consumed441 >= 115U * BUFFER_FRAMES / 128U && consumed441 <= 119U * BUFFER_FRAMES / 128U);

    // A disconnected slot renders silence.
    phoneA.stop();
    btRuntime->outputStorage[0][0][0] = 1.0f;
    renderSourceNode(context.get(), btRuntime.get(), nullptr, 0);
    assert(btRuntime->outputStorage[0][0][0] == 0.0f);
    phoneB.stop();

    btRuntime->sinkNodes[0] = 2;
    btRuntime->sinkNodeCount = 1;
    btRuntime->outputStorage[2][0][0] = 0.5f;
    context->nodeGainAtomics[2].store(1.0f);
    BluetoothOutputStream &headphones = context->bluetoothOutputs[1];
    headphones.setActive(true);
    headphones.signalActive.store(1);
    headphones.peer.store(btRuntime->btAddress[0]);
    servicePlaybackIoImpl<false>(context.get(), btRuntime.get(), 0);
    assert(headphones.available() == 0);
    assert(headphones.signalActive.load() == 0);
    headphones.peer.store(btRuntime->btAddress[2]);
    servicePlaybackIoImpl<false>(context.get(), btRuntime.get(), 0);
    assert(headphones.available() == BUFFER_FRAMES);
    assert(headphones.signalActive.load() == 1);
    int16_t packet[64 * 2];
    headphones.updateRate(64, 1000);
    assert(headphones.resample(packet, 64) == 64);
    assert(packet[0] > 0);
    headphones.setActive(false);
    assert(headphones.available() == 0);
    puts("Per-device Bluetooth routing node tests passed");

    AudioGraphState helperGraph{};
    helperGraph.generation = 1;
    helperGraph.thingCount = 3;
    strcpy(helperGraph.things[0].id, "soundboard_out");
    helperGraph.things[0].outputs = 2;
    strcpy(helperGraph.things[1].id, "test_out");
    helperGraph.things[1].inputs = 2;
    strcpy(helperGraph.things[2].id, "routing_helper");
    helperGraph.things[2].inputs = helperGraph.things[2].outputs = 2;
    for (unsigned i = 0; i < 12; ++i) {
        auto &incoming = helperGraph.edges[helperGraph.edgeCount++];
        strcpy(incoming.src, "soundboard_out");
        strcpy(incoming.dst, "routing_helper");
        auto &outgoing = helperGraph.edges[helperGraph.edgeCount++];
        strcpy(outgoing.src, "routing_helper");
        strcpy(outgoing.dst, "test_out");
    }
    assert(audiox::graph::expandRoutingHelper(&helperGraph) == 16);
    auto helperRuntime = std::make_unique<RuntimeGraph>();
    auto helperCapture = std::make_unique<AlsaCaptureBackend>();
    helperCapture->reset();
    rebuildRuntimeGraph(context.get(), helperRuntime.get(), helperCapture.get(), helperGraph);
    assert(helperRuntime->sourceRouteCount == AUDIO_GRAPH_MAX_EDGES);
    assert(helperRuntime->processRouteCount == 0);
    assert(helperRuntime->processNodeCount == 0);
    context->sfxQueue[1] = {0, 0, 1.0f};
    context->sfxQueueWrite.store(2);
    context->sfxSlotRefs[0].store(1);
    processGraphBlock(context.get(), helperRuntime.get(), helperCapture.get());
    assert(helperRuntime->inputs[1][0][0] > 0.1f);
    assert(helperRuntime->inputs[1][1][0] == 0.0f);
    closeAllStreams(helperRuntime.get(), helperCapture.get());
    puts("Expanded helper paths render through the runtime without a second expansion");
}
