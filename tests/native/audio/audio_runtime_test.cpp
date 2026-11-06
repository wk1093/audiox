#include "../../../src/audio/processing.cpp"
#include "audio/routing_compile.hpp"

#include <cassert>
#include <limits>
#include <vector>
#include <chrono>
#include <thread>

struct _snd_pcm { int token; };
static snd_pcm_t testPcm{};
static int16_t captureSample;
static unsigned captureReads;
static unsigned playbackWrites;
static int16_t playbackSample;
static bool modelPlayback = false;
static snd_pcm_state_t modeledState = SND_PCM_STATE_PREPARED;
static snd_pcm_uframes_t modeledQueued = 0, modeledBuffer = 0;
static snd_pcm_uframes_t modeledStartThreshold = 0;
static unsigned modeledStarts = 0, modeledRecoveries = 0;
static int16_t modeledMmapSamples[BUFFER_FRAMES * 2U]{};
static snd_pcm_channel_area_t modeledAreas[2] = {
    {modeledMmapSamples, 0, 32}, {modeledMmapSamples, 16, 32}
};

extern "C" snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buffer, snd_pcm_uframes_t frames) {
    assert(pcm == &testPcm);
    ++captureReads;
    std::fill_n(static_cast<int16_t *>(buffer), frames * 2U, captureSample);
    return static_cast<snd_pcm_sframes_t>(frames);
}

extern "C" snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *pcm, const void *buffer, snd_pcm_uframes_t frames) {
    assert(pcm == &testPcm);
    ++playbackWrites;
    playbackSample = static_cast<const int16_t *>(buffer)[0];
    if (modelPlayback) {
        if (modeledState == SND_PCM_STATE_XRUN) return -EPIPE;
        if (modeledQueued + frames > modeledBuffer) return -EAGAIN;
        modeledQueued += frames;
    }
    return static_cast<snd_pcm_sframes_t>(frames);
}

extern "C" snd_pcm_state_t snd_pcm_state(snd_pcm_t *) {
    return modelPlayback ? modeledState : SND_PCM_STATE_RUNNING;
}

extern "C" snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *) {
    assert(modelPlayback);
    return modeledState == SND_PCM_STATE_XRUN ? -EPIPE
        : static_cast<snd_pcm_sframes_t>(modeledBuffer - modeledQueued);
}

extern "C" int snd_pcm_start(snd_pcm_t *) {
    assert(modelPlayback && modeledState == SND_PCM_STATE_PREPARED);
    assert(modeledQueued >= modeledStartThreshold);
    ++modeledStarts;
    modeledState = SND_PCM_STATE_RUNNING;
    return 0;
}

extern "C" int snd_pcm_recover(snd_pcm_t *, int err, int) {
    assert(modelPlayback && err == -EPIPE);
    ++modeledRecoveries;
    modeledState = SND_PCM_STATE_PREPARED;
    modeledQueued = 0;
    return 0;
}

extern "C" int snd_pcm_mmap_begin(snd_pcm_t *, const snd_pcm_channel_area_t **areas,
                                  snd_pcm_uframes_t *offset, snd_pcm_uframes_t *frames) {
    assert(modelPlayback);
    *areas = modeledAreas;
    *offset = 0;
    *frames = std::min(*frames, modeledBuffer - modeledQueued);
    return 0;
}

extern "C" snd_pcm_sframes_t snd_pcm_mmap_commit(snd_pcm_t *, snd_pcm_uframes_t,
                                                snd_pcm_uframes_t frames) {
    assert(modelPlayback);
    modeledQueued += frames;
    return static_cast<snd_pcm_sframes_t>(frames);
}

static void testAdaptivePlaybackPriming() {
    auto context = std::make_unique<AudioContext>(nullptr);
    auto runtime = std::make_unique<RuntimeGraph>();
    runtime->snapshot.thingCount = 1;
    runtime->snapshot.things[0].inputs = 2;
    runtime->playbackCount = 1;
    auto &output = runtime->playback[0];
    output.pcm = &testPcm;
    output.channels = 2;
    output.format = SND_PCM_FORMAT_S16_LE;
    runtime->inputs[0][0] = runtime->silence;
    runtime->inputs[0][1] = runtime->silence;

    modelPlayback = true;
    for (bool mmap : {false, true}) {
        output.mmapAccess = mmap;
        for (unsigned period = BUFFER_FRAMES; period <= 1024; period *= 2) {
            output.periodFrames = period;
            output.bufferFrames = period * 4U;
            modeledBuffer = output.bufferFrames;
            modeledQueued = 0;
            modeledState = SND_PCM_STATE_PREPARED;
            modeledStarts = modeledRecoveries = 0;
            output.pendingFrames = output.pendingOffsetFrames = 0;
            const snd_pcm_uframes_t target = period == BUFFER_FRAMES
                ? BUFFER_FRAMES : period * 3U;
            assert(audio_pcm_playback_start_frames(period, modeledBuffer) == target);
            modeledStartThreshold = target;
            unsigned runningBlocks = 0;
            auto process = [&]() {
                servicePlaybackIo(context.get(), runtime.get());
                if (modeledState == SND_PCM_STATE_RUNNING) {
                    if (++runningBlocks % (period / BUFFER_FRAMES) == 0) {
                        assert(modeledQueued >= period); // No self-induced underrun at a period tick.
                        modeledQueued -= period;
                    }
                } else {
                    assert(modeledQueued < target);
                }
            };
            for (unsigned i = 0; i < 128; ++i) process();
            assert(modeledState == SND_PCM_STATE_RUNNING && modeledStarts == 1);
            assert(modeledRecoveries == 0);

            modeledState = SND_PCM_STATE_XRUN; // A real overload must re-prime, not restart empty.
            process();
            assert(modeledRecoveries == 1);
            runningBlocks = 0;
            for (unsigned i = 0; i < 128; ++i) process();
            assert(modeledState == SND_PCM_STATE_RUNNING && modeledStarts == 2);
            assert(modeledRecoveries == 1);
        }
    }
    modelPlayback = false;
    puts("Adaptive playback primes and recovers at every tier through RW and MMAP");
}

AudioContext::AudioContext(Audiox *context) : app(context) {}
AudioContext::~AudioContext() {}
int AudioContext::getEffectParams(const char *id, audiox::effects::SlotParams *out) const {
    auto it = effectStates.find(id);
    if (it == effectStates.end()) return RET_ERR;
    *out = it->second;
    return RET_OK;
}

static void testIdleSleep() {
    auto context = std::make_unique<AudioContext>(nullptr);
    auto runtime = std::make_unique<RuntimeGraph>();
    auto capture = std::make_unique<AlsaCaptureBackend>();
    context->pendingSfxStopAll.store(0);
    context->pendingSfxHoldStops.store(0);
    context->sfxQueueRead.store(0);
    context->sfxQueueWrite.store(0);
    context->sfxIsPlaying.store(0);
    context->sfxPlayingSeq.store(0);
    context->sfxPlayingCount = 0;
    context->effectStatesSeq.store(0);
    context->sfxSlots[0] = {};
    context->sfxSlotRefs[0].store(0);
    capture->reset();
    runtime->holdVoiceIndex = -1;
    runtime->snapshot.thingCount = 3;
    strcpy(runtime->snapshot.things[0].id, "mic_in");
    runtime->snapshot.things[0].outputs = 2;
    strcpy(runtime->snapshot.things[1].id, "idle_effect");
    runtime->snapshot.things[1].inputs = runtime->snapshot.things[1].outputs = 2;
    strcpy(runtime->snapshot.things[2].id, "test_out");
    runtime->snapshot.things[2].inputs = 2;
    runtime->nodeKind[0] = NODE_SOURCE;
    runtime->nodeKind[1] = NODE_EFFECT;
    runtime->nodeKind[2] = NODE_SINK;
    runtime->sourceNodes[0] = 0;
    runtime->sourceNodeCount = 1;
    runtime->processNodes[0] = 1;
    runtime->processNodeCount = 1;
    runtime->sinkNodes[0] = 2;
    runtime->sinkNodeCount = 1;
    runtime->sourceRoutes[0] = {0, 1, 0, 0};
    runtime->sourceRoutes[1] = {0, 1, 1, 1};
    runtime->sourceRouteCount = 2;
    runtime->processRoutes[0] = {1, 2, 0, 0};
    runtime->processRoutes[1] = {1, 2, 1, 1};
    runtime->processRouteCount = 2;
    buildProcessRouteIndex(runtime.get());
    for (unsigned node = 0; node < 3; ++node) context->nodeGainAtomics[node].store(1.0f);
    auto &effect = context->effectStates["idle_effect"];
    effect.enabled = 1;
    effect.type = audiox::effects::EFFECT_CUT;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);

    capture->count = 1;
    capture->nodeToStream[0] = 0;
    auto &input = capture->streams[0];
    input.pcm = &testPcm;
    input.channels = 2;
    input.format = SND_PCM_FORMAT_S16_LE;
    initAdaptiveSrc(&input.src, 0.5f, 1.0f);
    runtime->playbackCount = 1;
    auto &output = runtime->playback[0];
    output.pcm = &testPcm;
    output.nodeIndex = 2;
    output.channels = 2;
    output.format = SND_PCM_FORMAT_S16_LE;

    captureSample = 16384;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 100);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 5099);
    assert(!runtime->sleeping && runtime->dspBlocksProcessed == 2);
    auto hiddenReverb = effect;
    hiddenReverb.type = audiox::effects::EFFECT_REVERB;
    audiox::effects::setSlotDefaultsForType(&hiddenReverb, hiddenReverb.type);
    float hiddenInput[BUFFER_FRAMES]{};
    float hiddenOutput[BUFFER_FRAMES]{};
    hiddenInput[0] = 1.0f;
    audiox::effects::processSlot("idle_effect", 0, hiddenReverb,
                                 hiddenInput, hiddenOutput, BUFFER_FRAMES);
    assert(audiox::effects::slotHasPendingAudio("idle_effect", 0, hiddenReverb));
    output.pendingFrames = BUFFER_FRAMES;
    output.pendingBlock[0] = 16384;
    FILE *sleepLogs = tmpfile();
    assert(sleepLogs);
    fflush(stdout);
    const int savedStdout = dup(STDOUT_FILENO);
    assert(savedStdout >= 0);
    assert(dup2(fileno(sleepLogs), STDOUT_FILENO) >= 0);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 5100);
    assert(runtime->sleeping && runtime->dspBlocksProcessed == 2);
    assert(playbackSample == 0);
    assert(!audiox::effects::slotHasPendingAudio("idle_effect", 0, hiddenReverb));
    const unsigned readsBefore = captureReads;
    const unsigned writesBefore = playbackWrites;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 5200);
    assert(runtime->sleeping && runtime->dspBlocksProcessed == 2);
    assert(captureReads > readsBefore && playbackWrites == writesBefore + 1);
    assert(input.ring.count() < kCaptureRingFrames);
    assert(playbackSample == 0);
    assert(context->nodeChannelLevels[0][0].load() > 0.4f);
    assert(context->nodeChannelLevels[2][0].load() == 0.0f);

    auto &headphones = context->bluetoothOutputs[0];
    runtime->btAddress[2] = 0x112233445566ULL;
    headphones.peer.store(runtime->btAddress[2]);
    headphones.setActive(true);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 5200);
    assert(runtime->sleeping && headphones.available() == BUFFER_FRAMES);
    assert(!headphones.signalActive.load());
    headphones.setActive(false);
    runtime->btAddress[2] = 0;

    // Unmute must render the current captured block, not one block of stale silence.
    effect.enabled = 0;
    context->effectStatesSeq.fetch_add(1);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 5201);
    assert(!runtime->sleeping && runtime->dspBlocksProcessed == 3);
    assert(playbackSample > 15000);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 5202);
    fflush(stdout);
    assert(dup2(savedStdout, STDOUT_FILENO) >= 0);
    assert(close(savedStdout) == 0);
    rewind(sleepLogs);
    assert(fgetc(sleepLogs) == EOF);
    assert(!ferror(sleepLogs) && feof(sleepLogs));
    assert(fclose(sleepLogs) == 0);

    // Any live fan-out path, including another channel, prevents sleep.
    effect.enabled = 1;
    context->effectStatesSeq.fetch_add(1);
    runtime->sourceRoutes[2] = {0, 2, 1, 1};
    runtime->sourceRouteCount = 3;
    runtime->idleControlsKnown = false;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 6000);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 12000);
    assert(!runtime->sleeping && runtime->inputs[2][1][0] > 0.4f);
    runtime->sourceRouteCount = 2;
    runtime->idleControlsKnown = false;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 12001);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 17001);
    assert(runtime->sleeping);

    // Volume changes wake immediately; a zero-volume source may go idle.
    effect.enabled = 0;
    context->effectStatesSeq.fetch_add(1);
    context->nodeGainAtomics[0].store(0.0f);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 17002);
    assert(!runtime->sleeping);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 22002);
    assert(runtime->sleeping);
    context->nodeGainAtomics[0].store(1.0f);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 22003);
    assert(!runtime->sleeping && playbackSample > 15000);

    // Reproduce low-level analog noise on a live route alongside the muted mic.
    captureSample = 29;
    input.ring.reset();
    processGraphBlock(context.get(), runtime.get(), capture.get(), 22004);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 27003);
    assert(runtime->sleeping);
    captureSample = 1;
    input.ring.reset();
    processGraphBlock(context.get(), runtime.get(), capture.get(), 27004);
    assert(runtime->sleeping);
    captureSample = 33;
    input.ring.reset();
    processGraphBlock(context.get(), runtime.get(), capture.get(), 27005);
    assert(!runtime->sleeping && runtime->inputs[2][0][0] > kAudioIdleNoiseFloor);

    std::fill_n(runtime->outputs[0][0], BUFFER_FRAMES, kAudioIdleNoiseFloor);
    std::fill_n(runtime->outputs[0][1], BUFFER_FRAMES, kAudioIdleNoiseFloor);
    assert(!idleSourcesActive(runtime.get()));
    runtime->outputs[0][0][0] = std::nextafter(kAudioIdleNoiseFloor, 1.0f);
    assert(idleSourcesActive(runtime.get()));
    runtime->outputs[0][0][0] = std::numeric_limits<float>::quiet_NaN();
    assert(idleSourcesActive(runtime.get()));

    // A reverb's delay memory must prevent sleep even in an output-silent gap.
    effect.enabled = 1;
    effect.type = audiox::effects::EFFECT_REVERB;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    effect.values[3] = 1.0f;
    context->effectStatesSeq.fetch_add(1);
    captureSample = 0;
    input.ring.reset();
    processGraphBlock(context.get(), runtime.get(), capture.get(), 30000);
    float impulse[BUFFER_FRAMES]{};
    float tail[BUFFER_FRAMES]{};
    impulse[0] = 1.0f;
    runtime->inputs[1][0] = impulse;
    processNode(context.get(), runtime.get(), 1);
    assert(audiox::effects::slotHasPendingAudio("idle_effect", 0, effect));
    runtime->lastActivityMs = 30000;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 35000);
    assert(!runtime->sleeping);
    bool heardTail = false;
    for (uint64_t block = 1; block <= 12000; ++block) {
        processGraphBlock(context.get(), runtime.get(), capture.get(),
                          35000 + block * BUFFER_FRAMES * 1000ULL / SAMPLE_RATE);
        heardTail |= context->nodeChannelLevels[2][0].load() > 0.00001f;
        if (runtime->sleeping) break;
    }
    assert(heardTail && runtime->sleeping);
    assert(!audiox::effects::slotHasPendingAudio("idle_effect", 0, effect));

    // Soundboard triggers wake and advance the voice even before a reverb emits it.
    input.pcm = nullptr;
    output.pcm = nullptr;
    capture->count = 0;
    capture->nodeToStream[0] = -1;
    runtime->playbackCount = 0;
    strcpy(runtime->snapshot.things[0].id, "soundboard_out");
    int16_t clipSamples[BUFFER_FRAMES * 2]{};
    std::fill_n(clipSamples, BUFFER_FRAMES * 2, 16384);
    auto &clip = context->sfxSlots[0];
    clip.loaded = 1;
    strcpy(clip.name, "idle-wake.wav");
    clip.pcm = clipSamples;
    clip.channels = 1;
    clip.frames = BUFFER_FRAMES * 2;
    clip.sampleRate = SAMPLE_RATE;
    context->sfxSlotRefs[0].store(1);
    context->sfxQueue[0] = {0, 0, 1.0f};
    context->sfxQueueWrite.store(1);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 70000);
    assert(!runtime->sleeping && runtime->soundboardVoices[0].pos == BUFFER_FRAMES);
    processGraphBlock(context.get(), runtime.get(), capture.get(), 70003);
    assert(!runtime->soundboardVoices[0].active);
    assert(context->sfxSlotRefs[0].load() == 0);

    // Publishing a new routing graph always leaves idle mode.
    runtime->sleeping = true;
    const AudioGraphState graph = runtime->snapshot;
    rebuildRuntimeGraph(context.get(), runtime.get(), capture.get(), graph);
    assert(!runtime->sleeping && !runtime->idleControlsKnown);

    // Pitch latency and overlap buffers are also accounted for, and reset on sleep.
    effect.type = audiox::effects::EFFECT_PITCH;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    assert(audiox::effects::setSlotParamValue(&effect, "semitones", 7) == RET_OK);
    audiox::effects::processSlot("idle_pitch", 0, effect, impulse, tail, BUFFER_FRAMES);
    assert(audiox::effects::slotHasPendingAudio("idle_pitch", 0, effect));
    assert(!audiox::effects::slotHasPendingAudio("idle_pitch", 0, effect, 2.0f));
    audiox::effects::resetSlotAudio("idle_pitch", 0);
    assert(!audiox::effects::slotHasPendingAudio("idle_pitch", 0, effect));
    effect.type = 255;
    assert(audiox::effects::slotHasPendingAudio("future_plugin", 0, effect));

    puts("Five-second audio sleep, noise floor, live I/O, route demand, wake and effect-tail tests passed");
}

static void testDirectProcessing() {
    audiox::capture::PcmCaptureRing<4, 2> ring;
    const int16_t samples[] = {1, -1, 2, -2, 3, -3, 4, -4, 5, -5};
    ring.push(samples, 2, 5);
    assert(ring.count() == 4 && ring.sample(ring.tail(), 0) == 2);
    assert(ring.sample(ring.tail(), 1) == -2);
    assert(ring.sample(ring.readFrameIndex(3), 0) == 5);
    ring.setReadFraction(0.25f);
    ring.consume(0.5f);
    assert(ring.count() == 4 && fabsf(ring.readFraction() - 0.75f) < 0.0001f);
    ring.reset();
    assert(ring.count() == 0 && ring.readFraction() == 0.0f);

    assert(audiox::pcm::toS16(0.00001f) == 0);
    assert(audiox::pcm::toS32(0.00001f) != 0);
    assert(audiox::pcm::toS32(std::numeric_limits<float>::quiet_NaN()) == 0);
    assert(audiox::pcm::toS32(2.0f) == 2147483647);

    auto context = std::make_unique<AudioContext>(nullptr);
    auto runtime = std::make_unique<RuntimeGraph>();
    runtime->snapshot.thingCount = 4;
    for (unsigned node = 0; node < 4; ++node) {
        runtime->snapshot.things[node].inputs = node < 2 ? 0 : 2;
        runtime->snapshot.things[node].outputs = node == 3 ? 0 : 2;
        context->nodeGainAtomics[node].store(node == 0 ? 0.5f : 1.0f);
    }
    strcpy(runtime->snapshot.things[2].id, "direct_effect");
    runtime->nodeKind[2] = NODE_EFFECT;
    auto &effect = runtime->effectParamsCache[2];
    effect.enabled = 1;
    effect.type = audiox::effects::EFFECT_GAIN;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    assert(audiox::effects::setSlotParamValue(&effect, "level", 3.0f) == RET_OK);
    assert(audiox::effects::setSlotParamValue(&effect, "unknown", 1.0f) == RET_ERR);
    assert(audiox::effects::setSlotParamValue(&effect, "level",
        std::numeric_limits<float>::quiet_NaN()) == RET_ERR);

    const auto render = [&]() {
        clearRuntimeBuffers(runtime.get());
        for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) {
            runtime->outputs[0][0][frame] = 0.1f;
            runtime->outputs[0][1][frame] = -0.1f;
            runtime->outputs[1][0][frame] = 0.2f;
            runtime->outputs[1][1][frame] = -0.2f;
        }
        for (uint8_t channel = 0; channel < 2; ++channel) {
            routeCompiledEdge(context.get(), runtime.get(), {0, 2, channel, channel});
            routeCompiledEdge(context.get(), runtime.get(), {1, 2, channel, channel});
        }
        processNode(context.get(), runtime.get(), 2);
        for (uint8_t channel = 0; channel < 2; ++channel) {
            routeCompiledEdge(context.get(), runtime.get(), {2, 3, channel, channel});
        }
    };
    const auto expect = [&](float magnitude) {
        for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) {
            assert(fabsf(runtime->inputs[3][0][frame] - magnitude) < 0.0001f);
            assert(fabsf(runtime->inputs[3][1][frame] + magnitude) < 0.0001f);
        }
    };
    render();
    expect(0.75f);
    effect.enabled = 0;
    render();
    expect(0.25f);
    assert(runtime->outputs[2][0] == runtime->inputs[2][0]);
    assert(runtime->outputs[2][1] == runtime->inputs[2][1]);
    assert(runtime->outputs[0][0][0] == .1f && runtime->outputs[1][0][0] == .2f);
    effect.enabled = 1;
    effect.type = audiox::effects::EFFECT_CUT;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    assert(audiox::effects::setSlotParamValue(&effect, "channels", 3.6f) == RET_OK);
    assert(effect.values[0] == 4.0f);
    render();
    expect(0.0f);
    effect.enabled = 0;
    render();
    expect(0.25f);
    assert(runtime->outputs[2][0] == runtime->inputs[2][0]);

    for (uint8_t type : {audiox::effects::EFFECT_COMPRESSOR, audiox::effects::EFFECT_LIMITER,
                        audiox::effects::EFFECT_EQ, audiox::effects::EFFECT_MULTIBAND,
                        audiox::effects::EFFECT_NOISE_REDUCTION}) {
        effect = {};
        effect.type = type;
        effect.enabled = 1;
        audiox::effects::setSlotDefaultsForType(&effect, type);
        assert(audiox::effects::isBuiltinEffectType(type));
        assert(audiox::effects::effectTypeFromString(audiox::effects::effectTypeToString(type)) == type);
        if (type == audiox::effects::EFFECT_LIMITER)
            assert(audiox::effects::setSlotParamValue(&effect, "ceiling_db", -18) == RET_OK);
        for (unsigned block = 0; block < 400; ++block) render();
        for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) {
            const float left = runtime->inputs[3][0][frame], right = runtime->inputs[3][1][frame];
            if (!(left > .02f && left <= .251f && fabsf(left + right) < 1e-5f))
                fprintf(stderr, "Builtin type %u frame %u: left=%g right=%g\n", type, frame, left, right);
            assert(std::isfinite(left) && std::isfinite(right));
            assert(left > .02f && left <= .251f && fabsf(left + right) < 1e-5f);
            if (type == audiox::effects::EFFECT_COMPRESSOR ||
                type == audiox::effects::EFFECT_MULTIBAND ||
                type == audiox::effects::EFFECT_LIMITER) assert(left < .18f);
            if (type == audiox::effects::EFFECT_EQ) assert(left == .25f);
        }
        effect.enabled = 0;
        render();
        expect(.25f);
        assert(!audiox::effects::slotHasPendingAudio("direct_effect", 0, effect));
        effect.enabled = 1;
        assert(!audiox::effects::slotHasPendingAudio("direct_effect", 0, effect));
    }
    puts("All five new builtins execute through graph routes with symmetric independent channels and clean bypass/reset");

    float dirty[BUFFER_FRAMES]{}, clean[BUFFER_FRAMES]{};
    dirty[0] = std::numeric_limits<float>::quiet_NaN();
    dirty[1] = std::numeric_limits<float>::infinity();
    for (uint8_t type = 0; type <= audiox::effects::EFFECT_NOISE_REDUCTION; ++type) {
        effect = {};
        effect.type = type;
        effect.enabled = 1;
        audiox::effects::setSlotDefaultsForType(&effect, type);
        audiox::effects::processSlot("finite_builtin", 0, effect, dirty, clean, BUFFER_FRAMES);
        for (float sample : clean) assert(std::isfinite(sample));
        audiox::effects::resetSlotAudio("finite_builtin", 0);
        assert(!audiox::effects::slotHasPendingAudio("finite_builtin", 0, effect));
    }
    effect = {};
    effect.enabled = 1;
    effect.type = audiox::effects::EFFECT_REVERB;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    std::fill_n(dirty, BUFFER_FRAMES, 0);
    dirty[0] = .5f;
    audiox::effects::processSlot("changed_type", 0, effect, dirty, clean, BUFFER_FRAMES);
    assert(audiox::effects::slotHasPendingAudio("changed_type", 0, effect));
    effect.type = audiox::effects::EFFECT_GAIN;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    audiox::effects::processSlot("changed_type", 0, effect, dirty, clean, BUFFER_FRAMES);
    effect.type = audiox::effects::EFFECT_REVERB;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    assert(!audiox::effects::slotHasPendingAudio("changed_type", 0, effect));
    audiox::effects::processSlot("changed_type", 0, effect, dirty, clean, BUFFER_FRAMES);
    assert(audiox::effects::slotHasPendingAudio("changed_type", 0, effect));
    auto cutParams = effect;
    cutParams.type = audiox::effects::EFFECT_CUT;
    for (unsigned node = 0; node < 63; ++node) {
        char id[32];
        snprintf(id, sizeof(id), "bank_cut_%u", node);
        for (uint8_t channel = 0; channel < 16; ++channel)
            audiox::effects::prepareSlotAudio(id, channel, cutParams);
    }
    audiox::effects::prepareSlotAudio("changed_type", 0, effect);
    assert(audiox::effects::slotHasPendingAudio("changed_type", 0, effect));
    audiox::effects::resetSlotAudio("changed_type", 0);

    // Unity first contributions alias the source until another source mixes in.
    context->nodeGainAtomics[0].store(1.0f);
    clearRuntimeBuffers(runtime.get());
    runtime->outputs[0][0][0] = 0.1f;
    runtime->outputs[1][0][0] = 0.2f;
    routeCompiledEdge(context.get(), runtime.get(), {0, 2, 0, 0});
    assert(runtime->inputs[2][0] == runtime->outputs[0][0]);
    routeCompiledEdge(context.get(), runtime.get(), {1, 2, 0, 0});
    assert(runtime->inputs[2][0] == runtime->inputStorage[2][0]);
    assert(fabsf(runtime->inputs[2][0][0] - 0.3f) < 0.0001f);
    assert(runtime->outputs[0][0][0] == .1f && runtime->outputs[1][0][0] == .2f);

    effect.type = audiox::effects::EFFECT_REVERB;
    effect.enabled = 1;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    clearRuntimeBuffers(runtime.get());
    runtime->outputs[0][0][0] = .1f;
    runtime->outputs[1][0][0] = -.1f;
    routeCompiledEdge(context.get(), runtime.get(), {0, 2, 0, 0});
    routeCompiledEdge(context.get(), runtime.get(), {1, 2, 0, 0});
    const uint64_t cancellationBefore = runtime->effectChannelsProcessed;
    processNode(context.get(), runtime.get(), 2);
    assert(runtime->outputs[2][0] == runtime->silence);
    assert(runtime->effectChannelsProcessed == cancellationBefore);

    effect.type = audiox::effects::EFFECT_GAIN;
    effect.enabled = 1;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    clearRuntimeBuffers(runtime.get());
    runtime->outputs[0][0][0] = 1e-9f;
    routeCompiledEdge(context.get(), runtime.get(), {0, 2, 0, 0});
    processNode(context.get(), runtime.get(), 2);
    assert(runtime->outputs[2][0][0] == 1e-9f);
    assert(runtime->effectChannelsProcessed == cancellationBefore + 1);
    assert(audiox::effects::setSlotParamValue(&effect, "level", 4.0f) == RET_OK);
    render();
    expect(1.0f);
    effect.enabled = 0;
    context->nodeGainAtomics[0].store(4.0f);
    context->nodeGainAtomics[1].store(4.0f);
    render();
    expect(1.2f);

    effect.type = audiox::effects::EFFECT_DISTORTION;
    audiox::effects::setSlotDefaultsForType(&effect, effect.type);
    assert(audiox::effects::setSlotParamValue(&effect, "drive", 6.0f) == RET_OK);
    assert(effect.values[1] == 6.0f);
    assert(audiox::effects::setSlotParamValue(&effect, "drive",
        std::numeric_limits<float>::quiet_NaN()) == RET_ERR);
    puts("Direct runtime mixing, channel isolation, bypass, Cut, gain and PCM tests passed");
}

static void testGraphFastPaths() {
    auto context = std::make_unique<AudioContext>(nullptr);
    auto runtime = std::make_unique<RuntimeGraph>();
    auto capture = std::make_unique<AlsaCaptureBackend>();
    runtime->snapshot.thingCount = 9;
    runtime->sourceNodes[0] = 0;
    runtime->sourceNodeCount = 1;
    runtime->sinkNodes[0] = 8;
    runtime->sinkNodeCount = 1;
    runtime->nodeKind[0] = NODE_SOURCE;
    runtime->nodeKind[8] = NODE_SINK;
    strcpy(runtime->snapshot.things[0].id, "mic_in");
    strcpy(runtime->snapshot.things[8].id, "fast_path_out");
    runtime->snapshot.things[0].outputs = runtime->snapshot.things[8].inputs = 2;
    const uint8_t types[]{audiox::effects::EFFECT_CUT, audiox::effects::EFFECT_PITCH,
        audiox::effects::EFFECT_REVERB, audiox::effects::EFFECT_NOISE_REDUCTION,
        audiox::effects::EFFECT_LIMITER, audiox::effects::EFFECT_EQ, audiox::effects::EFFECT_GAIN};
    for (uint16_t node = 1; node <= 7; ++node) {
        auto &thing = runtime->snapshot.things[node];
        snprintf(thing.id, sizeof(thing.id), "fast_path_%u", node);
        thing.inputs = thing.outputs = 2;
        runtime->nodeKind[node] = NODE_EFFECT;
        runtime->processNodes[runtime->processNodeCount++] = node;
        auto &params = context->effectStates[thing.id];
        params.type = types[node - 1];
        params.enabled = 1;
        audiox::effects::setSlotDefaultsForType(&params, params.type);
    }
    auto &mute = context->effectStates["fast_path_1"];
    auto &pitch = context->effectStates["fast_path_2"];
    pitch.values[1] = 7;
    auto &independent = context->effectStates["fast_path_7"];
    independent.values[0] = .5f;
    for (unsigned node = 0; node < 9; ++node) context->nodeGainAtomics[node].store(1);
    runtime->sourceRoutes[runtime->sourceRouteCount++] = {0, 1, 0, 0};
    runtime->sourceRoutes[runtime->sourceRouteCount++] = {0, 7, 1, 1};
    for (uint16_t node = 1; node <= 6; ++node)
        runtime->processRoutes[runtime->processRouteCount++] = {node, uint16_t(node == 6 ? 8 : node + 1), 0, 0};
    runtime->processRoutes[runtime->processRouteCount++] = {7, 8, 1, 1};
    buildProcessRouteIndex(runtime.get());
    capture->reset();
    capture->count = 1;
    capture->nodeToStream[0] = 0;
    auto &input = capture->streams[0];
    input.pcm = &testPcm;
    input.channels = 2;
    input.format = SND_PCM_FORMAT_S16_LE;
    initAdaptiveSrc(&input.src, .5f, 1);
    captureSample = 16384;
    for (uint16_t node = 1; node <= 6; ++node) {
        std::fill_n(runtime->inputStorage[node][0], BUFFER_FRAMES, 123.f);
        std::fill_n(runtime->outputStorage[node][0], BUFFER_FRAMES, 456.f);
    }
    processGraphBlock(context.get(), runtime.get(), capture.get(), 100);
    assert(runtime->effectChannelsProcessed == 1);
    assert(!runtime->sleeping);
    for (uint16_t node = 1; node <= 6; ++node) {
        assert(runtime->inputs[node][0] == runtime->silence);
        assert(runtime->outputs[node][0] == runtime->silence);
        assert(runtime->inputContributionCount[node][0] == 0);
        assert(runtime->inputStorage[node][0][0] == 123);
        assert(runtime->outputStorage[node][0][0] == 456);
    }
    assert(runtime->inputs[8][0] == runtime->silence);
    assert(context->nodeChannelLevels[0][0].load() > .4f);
    assert(fabsf(runtime->inputs[8][1][0] - .25f) < 1e-5f);
    const uint64_t before = runtime->effectChannelsProcessed;
    for (unsigned block = 0; block < 20; ++block)
        processGraphBlock(context.get(), runtime.get(), capture.get(), 101 + block);
    assert(runtime->effectChannelsProcessed == before + 20);

    // Unmuted Cut aliases the source; bypass chains preserve identity and node gains.
    mute.enabled = 0;
    for (uint16_t node = 2; node <= 6; ++node)
        context->effectStates[runtime->snapshot.things[node].id].enabled = 0;
    context->nodeGainAtomics[2].store(.5f);
    context->effectStatesSeq.fetch_add(1);
    const uint64_t bypassBefore = runtime->effectChannelsProcessed;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 200);
    assert(runtime->effectChannelsProcessed == bypassBefore + 1);
    assert(runtime->outputs[1][0] == runtime->outputs[0][0]);
    assert(runtime->outputs[2][0] == runtime->outputs[0][0]);
    for (uint16_t node = 3; node <= 6; ++node)
        assert(runtime->outputs[node][0] == runtime->inputStorage[3][0]);
    assert(fabsf(runtime->inputs[8][0][0] - .25f) < 1e-5f);
    assert(runtime->inputStorage[2][0][0] == 123);
    assert(runtime->outputStorage[2][0][0] == 456);

    // Zero volume stops graph DSP without stopping source capture.
    mute.enabled = 1;
    independent.enabled = 1;
    context->nodeGainAtomics[0].store(0);
    context->effectStatesSeq.fetch_add(1);
    const uint64_t zeroGainBefore = runtime->effectChannelsProcessed;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 210);
    assert(runtime->effectChannelsProcessed == zeroGainBefore);
    assert(runtime->outputs[7][1] == runtime->silence);
    context->nodeGainAtomics[0].store(1);
    context->effectStatesSeq.fetch_add(1);

    // A downstream reverb keeps its audible tail after an upstream Cut closes.
    mute.enabled = 0;
    for (uint16_t node = 2; node <= 6; ++node)
        context->effectStates[runtime->snapshot.things[node].id].enabled = 1;
    context->nodeGainAtomics[2].store(1);
    context->effectStatesSeq.fetch_add(1);
    for (unsigned block = 0; block < 60; ++block)
        processGraphBlock(context.get(), runtime.get(), capture.get(), 300 + block);
    assert(audiox::effects::slotHasPendingAudio("fast_path_3", 0, runtime->effectParamsCache[3]));
    mute.enabled = 1;
    context->effectStatesSeq.fetch_add(1);
    const uint64_t tailBefore = runtime->effectChannelsProcessed;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 400);
    assert(runtime->effectChannelsProcessed > tailBefore + 1);
    assert(runtime->outputs[1][0] == runtime->silence);
    bool audibleTail = false;
    for (unsigned block = 0; block < 4000; ++block) {
        processGraphBlock(context.get(), runtime.get(), capture.get(), 401 + block);
        for (float sample : runtime->outputStorage[3][0])
            audibleTail |= fabsf(sample) > .001f;
    }
    assert(audibleTail && !runtime->sleeping);
    const uint64_t settled = runtime->effectChannelsProcessed;
    processGraphBlock(context.get(), runtime.get(), capture.get(), 4500);
    assert(runtime->effectChannelsProcessed == settled + 1);
    for (uint16_t node = 1; node <= 6; ++node)
        assert(runtime->outputs[node][0] == runtime->silence);
    puts("Cut disconnects a five-effect chain immediately; bypass aliases buffers, gains persist, tails drain and independent routes keep running");
}

static void testSilentRelease() {
    auto context = std::make_unique<AudioContext>(nullptr);
    auto runtime = std::make_unique<RuntimeGraph>();
    runtime->snapshot.thingCount = 2;
    runtime->snapshot.things[0].outputs = 1;
    runtime->snapshot.things[1].inputs = runtime->snapshot.things[1].outputs = 1;
    strcpy(runtime->snapshot.things[1].id, "release_graph");
    runtime->nodeKind[0] = NODE_SOURCE;
    runtime->nodeKind[1] = NODE_EFFECT;
    context->nodeGainAtomics[0].store(1);
    float reference[BUFFER_FRAMES]{};
    for (uint8_t type : {audiox::effects::EFFECT_GATE, audiox::effects::EFFECT_COMPRESSOR,
                        audiox::effects::EFFECT_LIMITER, audiox::effects::EFFECT_MULTIBAND}) {
        auto &params = runtime->effectParamsCache[1];
        params.type = type;
        params.enabled = 1;
        audiox::effects::setSlotDefaultsForType(&params, type);
        const auto *release = audiox::effects::effectParamSpecFor(type, "release_ms");
        assert(audiox::effects::setSlotParamValue(&params, "release_ms", release->maxValue) == RET_OK);
        audiox::effects::resetSlotAudio("release_reference", 0);
        const auto render = [&](float amplitude) {
            clearRuntimeBuffers(runtime.get());
            std::fill_n(runtime->outputs[0][0], BUFFER_FRAMES, amplitude);
            routeCompiledEdge(context.get(), runtime.get(), {0, 1, 0, 0});
            processNode(context.get(), runtime.get(), 1);
            audiox::effects::processSlot("release_reference", 0, params,
                runtime->outputs[0][0], reference, BUFFER_FRAMES);
            for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame)
                assert(fabsf(runtime->outputs[1][0][frame] - reference[frame]) < 1e-5f);
        };
        for (unsigned block = 0; block < 200; ++block) render(.8f);
        bool settled = false;
        for (unsigned block = 0; block < 16000; ++block) {
            render(0);
            if (runtime->outputs[1][0] == runtime->silence) {
                settled = true;
                break;
            }
        }
        assert(settled);
        const uint64_t before = runtime->effectChannelsProcessed;
        for (unsigned block = 0; block < 10; ++block) render(0);
        assert(runtime->effectChannelsProcessed == before);
        render(.8f);
        assert(runtime->effectChannelsProcessed == before + 1);
    }
    puts("Gate, compressor, limiter and multiband releases match continuously running DSP and eventually stop, including maximum release times");
}

static void testMultichannelCut() {
    auto context = std::make_unique<AudioContext>(nullptr);
    auto runtime = std::make_unique<RuntimeGraph>();
    runtime->snapshot.thingCount = 3;
    runtime->nodeKind[0] = NODE_SOURCE;
    runtime->nodeKind[1] = NODE_EFFECT;
    runtime->nodeKind[2] = NODE_SINK;
    runtime->snapshot.things[0].outputs = 16;
    runtime->snapshot.things[1].inputs = runtime->snapshot.things[1].outputs = 16;
    runtime->snapshot.things[2].inputs = 16;
    strcpy(runtime->snapshot.things[1].id, "fast_cut_16");
    context->nodeGainAtomics[0].store(1);
    context->nodeGainAtomics[1].store(1);
    auto &params = runtime->effectParamsCache[1];
    params.type = audiox::effects::EFFECT_CUT;
    audiox::effects::setSlotDefaultsForType(&params, params.type);
    params.values[0] = 16;
    for (uint8_t enabled : {0, 1, 0}) {
        params.enabled = enabled;
        clearRuntimeBuffers(runtime.get());
        for (uint8_t channel = 0; channel < 16; ++channel) {
            runtime->outputs[0][channel][0] = (channel + 1) * .01f;
            routeCompiledEdge(context.get(), runtime.get(), {0, 1, channel, channel});
        }
        processNode(context.get(), runtime.get(), 1);
        for (uint8_t channel = 0; channel < 16; ++channel) {
            routeCompiledEdge(context.get(), runtime.get(), {1, 2, channel, channel});
            assert(runtime->inputs[2][channel] ==
                (enabled ? runtime->silence : runtime->outputs[0][channel]));
            assert(runtime->outputs[0][channel][0] == (channel + 1) * .01f);
        }
        assert(runtime->effectChannelsProcessed == 0);
    }
    puts("Sixteen-channel Cut toggles between direct aliases and disconnections without DSP");
}

static void testNativePlugin() {
    audiox::plugin::PackageMetadata metadata{};
    strcpy(metadata.id, "plugin_fixture");
    strcpy(metadata.name, "Runtime fixture");
    strcpy(metadata.version, "1");
    metadata.api = 1;
    metadata.paramCount = 1;
    strcpy(metadata.params[0].name, "gain");
    strcpy(metadata.params[0].label, "Gain");
    metadata.params[0].min = -2;
    metadata.params[0].max = 2;
    metadata.params[0].defaultValue = 1;
    std::string error;
    assert(audiox::plugin::registerPackage(metadata, error));
    const std::string worker = AUDIOX_PLUGIN_WORKER_PATH;
    const std::string module = worker.substr(0, worker.find_last_of('/')) + "/plugins/plugin-fixture-0.so";
    auto host = std::make_unique<audiox::plugin::PluginHost>();
    assert(host->prepare(module.c_str(), metadata, SAMPLE_RATE, BUFFER_FRAMES, 2, error, worker.c_str()));
    auto context = std::make_unique<AudioContext>(nullptr);
    auto runtime = std::make_unique<RuntimeGraph>();
    auto &thing = runtime->snapshot.things[0];
    strcpy(thing.id, "native_effect");
    thing.inputs = thing.outputs = 2;
    runtime->snapshot.thingCount = 1;
    runtime->nodeKind[0] = NODE_EFFECT;
    runtime->processNodes[0] = 0;
    runtime->processNodeCount = 1;
    runtime->demandedOutputs[0] = 3;
    auto &params = runtime->effectParamsCache[0];
    params.type = audiox::plugin::findType(metadata.id);
    params.enabled = 1;
    params.pluginHost = host.get();
    params.values[0] = 2;
    clearRuntimeBuffers(runtime.get());
    runtime->inputs[0][0] = runtime->inputStorage[0][0];
    runtime->inputs[0][1] = runtime->inputStorage[0][1];
    std::fill_n(runtime->inputs[0][0], BUFFER_FRAMES, 0.1f);
    std::fill_n(runtime->inputs[0][1], BUFFER_FRAMES, 0.3f);
    processNode(context.get(), runtime.get(), 0);
    assert(runtime->outputs[0][0][0] == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    processNode(context.get(), runtime.get(), 0);
    assert(fabsf(runtime->outputs[0][0][0] - 0.2f) < 0.00001f);
    assert(fabsf(runtime->outputs[0][1][0] - 0.6f) < 0.00001f);
    assert(idleEffectsPending(runtime.get()));
    assert(idleEffectsPending(runtime.get()));
    assert(host->hasPendingAudio(0.01f));
    params.enabled = 0;
    processNode(context.get(), runtime.get(), 0);
    assert(runtime->outputs[0][0][0] == 0.1f && runtime->outputs[0][1][0] == 0.3f);
    assert(runtime->outputs[0][0] == runtime->inputs[0][0]);
    assert(runtime->outputs[0][1] == runtime->inputs[0][1]);
    params.enabled = 1;
    beginAudioSleep(runtime.get());
    assert(!host->hasPendingAudio(0.01f));
    processNode(context.get(), runtime.get(), 0);
    assert(runtime->outputs[0][0][0] == 0);
    context->effectStates[thing.id] = params;
    context->effectStatesSeq.store(7);
    updateIdleControls(context.get(), runtime.get());
    assert(context->pluginObservedSeq.load() == 7);
    context->effectStates.erase(thing.id);
    context->effectStatesSeq.store(8);
    updateIdleControls(context.get(), runtime.get());
    assert(!runtime->effectParamsCache[0].pluginHost);
    assert(context->pluginObservedSeq.load() == 8);
    puts("Native plugin planar processing, stereo, bypass, sleep-reset, tails and retirement observation tests passed");
}

int main() {
    {
        auto context = std::make_unique<AudioContext>(nullptr);
        auto runtime = std::make_unique<RuntimeGraph>();
        runtime->snapshot.thingCount = 2;
        runtime->snapshot.things[0].inputs = 1;
        runtime->snapshot.things[1].inputs = 1;
        runtime->sinkNodeCount = 2;
        runtime->sinkNodes[0] = 0;
        runtime->sinkNodes[1] = 1;
        float source[BUFFER_FRAMES];
        std::fill_n(source, BUFFER_FRAMES, 1.0f);
        context->sceneMuted.store(true);
        for (unsigned block = 0; block < 8; ++block) {
            runtime->inputs[0][0] = source;
            runtime->inputs[1][0] = source;
            servicePlaybackIo(context.get(), runtime.get());
            assert(source[0] == 1.0f && source[BUFFER_FRAMES - 1] == 1.0f);
            for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame)
                assert(runtime->inputs[0][0][frame] == runtime->inputs[1][0][frame]);
            assert(runtime->inputs[0][0][0] >= runtime->inputs[0][0][BUFFER_FRAMES - 1]);
        }
        assert(context->sceneSilent.load() && runtime->sceneFade.gain == 0);
        context->sceneMuted.store(false);
        for (unsigned block = 0; block < 8; ++block) {
            runtime->inputs[0][0] = source;
            runtime->inputs[1][0] = source;
            servicePlaybackIo(context.get(), runtime.get());
            assert(runtime->inputs[0][0][0] <= runtime->inputs[0][0][BUFFER_FRAMES - 1]);
        }
        assert(!context->sceneSilent.load() && runtime->sceneFade.gain == 1);
        puts("Scene sink fades preserve shared source aliases and acknowledge exact silence");
    }
    testAdaptivePlaybackPriming();
    testDirectProcessing();
    testGraphFastPaths();
    testSilentRelease();
    testMultichannelCut();
    testIdleSleep();
    testNativePlugin();
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
    AudioContext::ScriptSoundEvent soundEvent{};
    assert(context->popScriptSoundEvent(&soundEvent) && soundEvent.started);
    assert(strcmp(soundEvent.sound, "test.wav") == 0);
    assert(context->popScriptSoundEvent(&soundEvent) && !soundEvent.started);
    assert(strcmp(soundEvent.sound, "test.wav") == 0);
    assert(!context->popScriptSoundEvent(&soundEvent));

    // A short voice can start and finish within one processing block.
    clip.frames = 1;
    context->sfxSlotRefs[0].store(1);
    context->sfxQueue[1] = {0, 0, 1.0f};
    context->sfxQueueWrite.store(2);
    processGraphBlock(context.get(), runtime.get(), capture.get());
    assert(context->popScriptSoundEvent(&soundEvent) && soundEvent.started);
    assert(context->popScriptSoundEvent(&soundEvent) && !soundEvent.started);
    assert(!context->popScriptSoundEvent(&soundEvent));

    clip.frames = BUFFER_FRAMES * 2;
    context->sfxSlotRefs[0].store(1);
    context->sfxQueue[2] = {0, 0, 1.0f};
    context->sfxQueueWrite.store(3);
    processGraphBlock(context.get(), runtime.get(), capture.get());
    assert(context->popScriptSoundEvent(&soundEvent) && soundEvent.started);
    context->pendingSfxStopAll.fetch_add(1);
    processGraphBlock(context.get(), runtime.get(), capture.get());
    assert(context->popScriptSoundEvent(&soundEvent) && !soundEvent.started);
    assert(!context->popScriptSoundEvent(&soundEvent));
    for (unsigned i = 0; i < 130; ++i) context->publishScriptSoundEvent("overflow.wav", i % 2 == 0);
    assert(context->scriptSoundDropped.exchange(0) == 2);
    for (unsigned i = 0; i < 128; ++i) {
        assert(context->popScriptSoundEvent(&soundEvent));
        assert(soundEvent.started == (i % 2 == 0));
    }
    assert(!context->popScriptSoundEvent(&soundEvent));
    context->sfxQueueRead.store(1);
    context->sfxQueueWrite.store(1);
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
    servicePlaybackIo(context.get(), btRuntime.get());
    assert(headphones.available() == 0);
    assert(headphones.signalActive.load() == 0);
    headphones.peer.store(btRuntime->btAddress[2]);
    servicePlaybackIo(context.get(), btRuntime.get());
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
