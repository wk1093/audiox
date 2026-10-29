#include "../src/audio/processing.cpp"

#include <cassert>

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
}
