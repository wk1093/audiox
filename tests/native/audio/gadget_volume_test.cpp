#include "audio/gadget_volume.hpp"
#include "../../../src/audio/processing.cpp"

#include <cassert>

struct _snd_ctl { int token; };
struct _snd_pcm { int token; };
static snd_ctl_t testControl = {};
static snd_pcm_t testPcm = {};
static int readError = 0;
static int openError = 0;
static long captureVolume = 96, playbackVolume = 96;
static long captureSwitch = 1, playbackSwitch = 1;
static unsigned reads = 0;
static int32_t outputSample = 0;
static snd_pcm_format_t outputFormat = SND_PCM_FORMAT_S16_LE;
static unsigned outputWrites = 0;

AudioContext::AudioContext(Audiox *context) : app(context) {}
AudioContext::~AudioContext() {}

extern "C" snd_pcm_sframes_t snd_pcm_writei(snd_pcm_t *, const void *buffer, snd_pcm_uframes_t frames) {
    ++outputWrites;
    outputSample = outputFormat == SND_PCM_FORMAT_S32_LE
        ? static_cast<const int32_t *>(buffer)[0] : static_cast<const int16_t *>(buffer)[0];
    return static_cast<snd_pcm_sframes_t>(frames);
}

extern "C" snd_pcm_state_t snd_pcm_state(snd_pcm_t *) { return SND_PCM_STATE_RUNNING; }

extern "C" snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t *) { return BUFFER_FRAMES * 4U; }

extern "C" int snd_ctl_open(snd_ctl_t **ctl, const char *, int mode) {
    assert(mode == SND_CTL_NONBLOCK);
    if (openError) return openError;
    *ctl = &testControl;
    return 0;
}

extern "C" int snd_ctl_close(snd_ctl_t *) { return 0; }

extern "C" int snd_ctl_elem_read(snd_ctl_t *, snd_ctl_elem_value_t *element) {
    ++reads;
    if (readError) return readError;
    assert(snd_ctl_elem_value_get_interface(element) == SND_CTL_ELEM_IFACE_MIXER);
    assert(snd_ctl_elem_value_get_device(element) == 0);
    const char *name = snd_ctl_elem_value_get_name(element);
    long value = 0;
    if (strcmp(name, "PCM Capture Volume") == 0) value = captureVolume;
    else if (strcmp(name, "PCM Playback Volume") == 0) value = playbackVolume;
    else if (strcmp(name, "PCM Capture Switch") == 0) value = captureSwitch;
    else if (strcmp(name, "PCM Playback Switch") == 0) value = playbackSwitch;
    else assert(false);
    snd_ctl_elem_value_set_integer(element, 0, value);
    return 0;
}

int main() {
    audiox::usb::GadgetVolume controls;
    assert(controls.captureGain.load() == 0.0f);
    controls.poll(2, 0, 0);
    assert(controls.captureGain.load() == 1.0f && controls.playbackGain.load() == 1.0f);
    unsigned previousReads = reads;
    controls.poll(2, 0, 49);
    assert(reads == previousReads);
    captureVolume = 76;
    playbackVolume = 90;
    controls.poll(2, 0, 50);
    assert(std::fabs(controls.captureGain.load() - 0.1f) < 0.00001f);
    assert(std::fabs(controls.playbackGain.load() - 0.5011872f) < 0.00001f);
    captureSwitch = 0;
    controls.poll(2, 0, 100);
    assert(controls.captureGain.load() == 0.0f && controls.playbackGain.load() > 0.5f);
    playbackSwitch = 0;
    controls.poll(2, 0, 150);
    assert(controls.playbackGain.load() == 0.0f);
    readError = -ENOENT;
    controls.poll(2, 0, 200);
    assert(controls.captureGain.load() == 0.0f && controls.playbackGain.load() == 0.0f);
    readError = 0;
    captureVolume = playbackVolume = 96;
    captureSwitch = playbackSwitch = 1;
    controls.poll(3, 0, 201);
    assert(controls.captureGain.load() == 1.0f);
    controls.poll(-1, 0, 202);
    assert(controls.captureGain.load() == 0.0f && controls.playbackGain.load() == 0.0f);
    controls.poll(3, 0, 203);
    controls.reset();
    assert(controls.captureGain.load() == 0.0f);
    controls.poll(3, 0, 204);
    captureVolume = 97;
    controls.poll(3, 0, 254);
    assert(controls.captureGain.load() == 0.0f);
    captureVolume = 96;
    controls.poll(3, 0, 1253);
    assert(controls.captureGain.load() == 0.0f);
    controls.poll(3, 0, 1254);
    assert(controls.captureGain.load() == 1.0f);
    openError = -ENODEV;
    controls.poll(3, 0, 1304);
    assert(controls.captureGain.load() == 0.0f && controls.playbackGain.load() == 0.0f);
    openError = 0;

    static AlsaCaptureStream capture = {};
    static RuntimeGraph::AlsaPlaybackStream playback = {};
    capture.channels = 2;
    playback.channels = 2;
    float data[2][BUFFER_FRAMES] = {};
    float *channels[] = {data[0], data[1]};
    const int16_t samples[] = {8192, -8192};
    for (float gain : {1.0f, 0.5f, 0.1f, 0.0f}) {
        capture.ring.reset();
        initAdaptiveSrc(&capture.src, 0.5f, 1.0f);
        capture.ring.push(samples, 2, 1);
        renderCaptureSource(capture, channels, 2, gain);
        // A one-frame source exhausts immediately: every later sample is underrun history.
        for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) {
            assert(std::fabs(data[0][frame] - 0.25f * gain) < 0.000001f);
            assert(std::fabs(data[1][frame] + 0.25f * gain) < 0.000001f);
        }
        renderCaptureSource(capture, channels, 2, gain);
        assert(std::fabs(data[0][0] - 0.25f * gain) < 0.000001f);
    }
    // Both output widths apply host attenuation once, before PCM quantization.
    for (snd_pcm_format_t format : {SND_PCM_FORMAT_S16_LE, SND_PCM_FORMAT_S32_LE}) {
        playback.format = format;
        for (float gain : {1.0f, 0.5f, 0.0f}) {
            resetPlaybackSrc(playback);
            for (unsigned frame = 0; frame < BUFFER_FRAMES; ++frame) {
                data[0][frame] = 0.25f;
                data[1][frame] = -0.25f;
            }
            preparePlaybackBlock(playback, channels, 2, gain);
            assert(playback.sourceCount == BUFFER_FRAMES);
            resamplePlaybackBlock(playback);
            assert(playback.pendingFrames == BUFFER_FRAMES - 1U);
            for (unsigned frame = 0; frame < playback.pendingFrames; ++frame) {
                if (format == SND_PCM_FORMAT_S16_LE) {
                    assert(playback.pendingBlock[frame * 2U] == audiox::pcm::toS16(0.25f * gain));
                    assert(playback.pendingBlock[frame * 2U + 1U] == audiox::pcm::toS16(-0.25f * gain));
                } else {
                    assert(playback.pendingBlock32[frame * 2U] == audiox::pcm::toS32(0.25f * gain));
                    assert(playback.pendingBlock32[frame * 2U + 1U] == audiox::pcm::toS32(-0.25f * gain));
                }
            }
        }
        // Exercise the real boundary call sites as well as their conversion helpers.
        auto context = std::make_unique<AudioContext>(nullptr);
        static RuntimeGraph graph = {};
        static AlsaCaptureBackend backend = {};
        graph.snapshot.thingCount = 1;
        strcpy(graph.snapshot.things[0].id, "usb_gadget_out");
        graph.snapshot.things[0].outputs = 2;
        graph.snapshot.things[0].inputs = 2;
        graph.outputs[0][0] = data[0];
        graph.outputs[0][1] = data[1];
        graph.inputs[0][0] = data[0];
        graph.inputs[0][1] = data[1];
        backend.count = 1;
        backend.nodeToStream[0] = 0;
        backend.streams[0].channels = 2;
        backend.streams[0].isGadget = 1;
        graph.playbackCount = 1;
        graph.playback[0].pcm = &testPcm;
        graph.playback[0].channels = 2;
        graph.playback[0].isGadget = 1;
        graph.playback[0].format = format;
        outputFormat = format;
        const auto expectedSample = [format](float sample) -> int32_t {
            return format == SND_PCM_FORMAT_S32_LE
                ? audiox::pcm::toS32(sample) : audiox::pcm::toS16(sample);
        };
        for (float gain : {1.0f, 0.1f, 0.0f}) {
            context->gadgetVolume.captureGain.store(gain);
            auto &source = backend.streams[0];
            source.ring.reset();
            initAdaptiveSrc(&source.src, 0.5f, 1.0f);
            source.ring.push(samples, 2, 1);
            renderSourceNode(context.get(), &graph, &backend, 0);
            assert(std::fabs(data[0][0] - 0.25f * gain) < 0.000001f);
            context->gadgetVolume.playbackGain.store(1.0f);
            resetPlaybackSrc(graph.playback[0]);
            unsigned previousWrites = outputWrites;
            servicePlaybackIo(context.get(), &graph);
            assert(outputWrites > previousWrites);
            assert(outputSample == expectedSample(0.25f * gain));
            context->gadgetVolume.playbackGain.store(gain);
            std::fill_n(data[0], BUFFER_FRAMES, 0.25f);
            resetPlaybackSrc(graph.playback[0]);
            previousWrites = outputWrites;
            servicePlaybackIo(context.get(), &graph);
            assert(outputWrites > previousWrites);
            assert(outputSample == expectedSample(0.25f * gain));
        }
    }
    puts("USB volume, mute, reconnect, PCM and underrun tests passed");
}
