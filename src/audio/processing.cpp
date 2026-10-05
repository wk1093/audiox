#include "audio/context.hpp"
#include "audio/alsa_pcm.h"
#include "audio/effects/slot.hpp"
#include "audio/processing_fx.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdint.h>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint32_t kMaxChannelsPerThing = 16;
constexpr float kSrcRatioMin = 0.97f;
constexpr float kSrcRatioMax = 1.03f;
constexpr float kSrcP = 0.035f;
constexpr float kSrcI = 0.003f;
constexpr uint32_t kMaxCaptureStreams = 4;
constexpr uint32_t kMaxPlaybackStreams = 4;
constexpr uint32_t kCaptureRingFrames = BUFFER_FRAMES * 16U;
constexpr uint32_t kReopenRetryBlocks = 200;
constexpr float kSoundboardClipGain = 0.35f;
constexpr float kBluetoothOutputPeakLimit = 0.2511886f;
constexpr uint32_t kMaxSoundboardVoices = 64;

struct AdaptiveSrcController {
    float ratio;
    float integral;
    float targetFill;
    float minRatio;
    float maxRatio;
    float pGain;
    float iGain;
    float baseRatio;
};

enum NodeKind {
    NODE_SOURCE = 0,
    NODE_EFFECT = 1,
    NODE_SINK = 2,
    NODE_PASS = 3,
};

struct RuntimeGraph {
    struct SoundboardVoice {
        uint8_t active;
        uint8_t hold;
        uint8_t slotIndex;
        uint8_t channels;
        uint32_t frames;
        uint32_t sampleRate;
        uint32_t pos;
        float frac;
        float pitchRatio;
        uint64_t startedAtBlock;
    };

    struct CompiledRoute {
        uint16_t srcNode;
        uint16_t dstNode;
        uint8_t srcChannel;
        uint8_t dstChannel;
    };

    struct AlsaCaptureStream {
        int active;
        snd_pcm_t *pcm;
        uint16_t nodeIndex;
        uint8_t channels;
        uint32_t card;
        uint32_t device;
        uint32_t sampleRate;
        uint32_t reopenRetryBlocks;
        uint32_t ringHead;
        uint32_t ringTail;
        uint32_t ringCount;
        float readFrac;
        snd_pcm_format_t format;
        int mmapAccess;
        AdaptiveSrcController src;
        float lastSample[kMaxChannelsPerThing];
        char path[64];
        int16_t ioBlock[BUFFER_FRAMES * kMaxChannelsPerThing];
        int32_t ioBlock32[BUFFER_FRAMES * kMaxChannelsPerThing];
        int16_t ring[kCaptureRingFrames * kMaxChannelsPerThing];
    };

    struct AlsaPlaybackStream {
        int active;
        snd_pcm_t *pcm;
        uint16_t nodeIndex;
        uint8_t channels;
        uint32_t card;
        uint32_t device;
        uint32_t sampleRate;
        uint32_t reopenRetryBlocks;
        uint32_t pendingFrames;
        uint32_t pendingOffsetFrames;
        snd_pcm_format_t format;
        int mmapAccess;
        char path[64];
        int16_t pendingBlock[BUFFER_FRAMES * kMaxChannelsPerThing];
        int32_t pendingBlock32[BUFFER_FRAMES * kMaxChannelsPerThing];
    };

    AudioGraphState snapshot;
    uint16_t nodeKind[AUDIO_GRAPH_MAX_THINGS];
    uint16_t sourceNodes[AUDIO_GRAPH_MAX_THINGS];
    uint16_t sourceNodeCount;
    uint16_t processNodes[AUDIO_GRAPH_MAX_THINGS];
    uint16_t processNodeCount;
    uint16_t sinkNodes[AUDIO_GRAPH_MAX_THINGS];
    uint16_t sinkNodeCount;
    CompiledRoute sourceRoutes[AUDIO_GRAPH_MAX_EDGES];
    uint16_t sourceRouteCount;
    CompiledRoute processRoutes[AUDIO_GRAPH_MAX_EDGES];
    CompiledRoute processRoutesBySrc[AUDIO_GRAPH_MAX_EDGES];
    uint16_t processRouteCount;
    uint16_t processRouteStart[AUDIO_GRAPH_MAX_THINGS];
    uint16_t processRouteLen[AUDIO_GRAPH_MAX_THINGS];
    float inputStorage[AUDIO_GRAPH_MAX_THINGS][kMaxChannelsPerThing][BUFFER_FRAMES];
    float outputStorage[AUDIO_GRAPH_MAX_THINGS][kMaxChannelsPerThing][BUFFER_FRAMES];
    float *inputs[AUDIO_GRAPH_MAX_THINGS][kMaxChannelsPerThing];
    float *outputs[AUDIO_GRAPH_MAX_THINGS][kMaxChannelsPerThing];
    uint8_t inputContributionCount[AUDIO_GRAPH_MAX_THINGS][kMaxChannelsPerThing];
    audiox::effects::SlotParams effectParamsCache[AUDIO_GRAPH_MAX_THINGS];
    uint32_t effectParamsSeq[AUDIO_GRAPH_MAX_THINGS];
    SoundboardVoice soundboardVoices[kMaxSoundboardVoices];
    int16_t holdVoiceIndex;
    int16_t nodeToCaptureStream[AUDIO_GRAPH_MAX_THINGS];
    int16_t nodeToPlaybackStream[AUDIO_GRAPH_MAX_THINGS];
    int16_t soundboardNodeIndex;
    AlsaCaptureStream capture[kMaxCaptureStreams];
    AlsaPlaybackStream playback[kMaxPlaybackStreams];
    uint16_t captureCount;
    uint16_t playbackCount;
    uint64_t blocksProcessed;
    uint64_t nextStatsBlock;
    uint32_t publishedPlayingCount;
    char publishedPlayingBasenames[AUDIO_SFX_SLOT_COUNT][MIDI_SFX_PATH_MAX];
    std::atomic<float> channelLevels[AUDIO_GRAPH_MAX_THINGS][kMaxChannelsPerThing];
};

static uint64_t monotonicMs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static timespec monotonicNow() {
    timespec ts = {};
    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts;
}

static void addNs(timespec *ts, uint64_t ns) {
    if (!ts) {
        return;
    }
    uint64_t nsec = (uint64_t)ts->tv_nsec + ns;
    ts->tv_sec += (time_t)(nsec / 1000000000ULL);
    ts->tv_nsec = (long)(nsec % 1000000000ULL);
}

static int cmpTimespec(const timespec &a, const timespec &b) {
    if (a.tv_sec < b.tv_sec) {
        return -1;
    }
    if (a.tv_sec > b.tv_sec) {
        return 1;
    }
    if (a.tv_nsec < b.tv_nsec) {
        return -1;
    }
    if (a.tv_nsec > b.tv_nsec) {
        return 1;
    }
    return 0;
}

static uint64_t blockPeriodNs() {
    return (1000000000ULL * (uint64_t)BUFFER_FRAMES) / (uint64_t)SAMPLE_RATE;
}

static void waitUntilBlockDeadline(timespec *deadline) {
    if (!deadline) {
        return;
    }

    // (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, nullptr);
    // addNs(deadline, blockPeriodNs());

    // timespec now = monotonicNow();
    // if (cmpTimespec(now, *deadline) > 0) {
    //     // If we overran, re-anchor to avoid accumulating wakeup drift.
    //     *deadline = now;
    //     addNs(deadline, blockPeriodNs());
    // }

    // Old timing above, newer improved timing mechanism

    (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, nullptr);
    timespec now = monotonicNow();

    long halfBlockNs = (long)(blockPeriodNs() / 2ULL);
    timespec thresh = *deadline;
    addNs(&thresh, halfBlockNs);
    if (cmpTimespec(now, thresh) > 0) {
        *deadline = now;
        addNs(deadline, blockPeriodNs());
    } else {
        addNs(deadline, blockPeriodNs());
    }

}

static void initAdaptiveSrc(AdaptiveSrcController *src, float initialFill, float baseRatio) {
    if (!src) {
        return;
    }
    if (baseRatio < 0.25f) {
        baseRatio = 1.0f;
    }

    src->ratio = baseRatio;
    src->integral = 0.0f;
    src->targetFill = initialFill;
    src->baseRatio = baseRatio;
    src->minRatio = baseRatio * kSrcRatioMin;
    src->maxRatio = baseRatio * kSrcRatioMax;
    src->pGain = kSrcP;
    src->iGain = kSrcI;
}

static float adaptiveSrcStep(AdaptiveSrcController *src, float fillNow) {
    if (!src) {
        return 1.0f;
    }

    // Positive error means ring is healthier than target, so we can read a bit faster.
    // Negative error means ring is low, so we must read slower to avoid underruns.
    float error = fillNow - src->targetFill;
    src->integral += error;
    if (src->integral > 1.0f) {
        src->integral = 1.0f;
    }
    if (src->integral < -1.0f) {
        src->integral = -1.0f;
    }

    float ratio = src->baseRatio + (src->pGain * error) + (src->iGain * src->integral);
    if (ratio < src->minRatio) {
        ratio = src->minRatio;
    }
    if (ratio > src->maxRatio) {
        ratio = src->maxRatio;
    }

    src->ratio = ratio;
    return ratio;
}

static bool parseThingCardDevice(const char *id,
                                 const char *pattern,
                                 char *canonical,
                                 size_t canonicalSize,
                                 uint32_t *card,
                                 uint32_t *device) {
    if (!id || !pattern || !card || !device || !canonical || canonicalSize == 0) {
        return false;
    }

    unsigned c = 0;
    unsigned d = 0;
    if (sscanf(id, pattern, &c, &d) != 2) {
        return false;
    }

    int n = snprintf(canonical, canonicalSize, pattern, c, d);
    if (n <= 0 || (size_t)n >= canonicalSize) {
        return false;
    }

    if (strcmp(canonical, id) != 0) {
        return false;
    }

    *card = (uint32_t)c;
    *device = (uint32_t)d;
    return true;
}

static bool parseCaptureThing(const char *id, uint32_t *card, uint32_t *device) {
    char canonical[64];
    return parseThingCardDevice(id,
                                "alsa_card%u_dev%u_in",
                                canonical,
                                sizeof(canonical),
                                card,
                                device);
}

static bool parsePlaybackThing(const char *id, uint32_t *card, uint32_t *device) {
    char canonical[64];
    return parseThingCardDevice(id,
                                "alsa_card%u_dev%u_out",
                                canonical,
                                sizeof(canonical),
                                card,
                                device);
}

static bool parseStableThing(const char *id,
                             char *stableCardId,
                             size_t stableCardIdSize,
                             uint32_t *device) {
    if (!id || !stableCardId || stableCardIdSize == 0 || !device) {
        return false;
    }

    const char *prefix = "alsa_";
    if (strncmp(id, prefix, strlen(prefix)) != 0) {
        return false;
    }

    // Find _dev{digit} — the device-index separator.
    // Must check for a trailing digit to avoid matching _dev inside a stableCardId
    // that starts with "dev" (e.g. "device").
    const char *devTag = nullptr;
    const char *p = id;
    while (*p) {
        const char *found = strstr(p, "_dev");
        if (!found) {
            break;
        }
        if (isdigit((unsigned char)found[4])) {
            devTag = found;
            break;
        }
        p = found + 1;
    }

    if (!devTag) {
        return false;
    }

    size_t slugLen = (size_t)(devTag - (id + strlen(prefix)));
    if (slugLen == 0 || slugLen >= stableCardIdSize) {
        return false;
    }

    memcpy(stableCardId, id + strlen(prefix), slugLen);
    stableCardId[slugLen] = '\0';

    unsigned parsedDevice = 0;
    if (sscanf(devTag, "_dev%u", &parsedDevice) != 1) {
        return false;
    }
    *device = (uint32_t)parsedDevice;
    return true;
}

static bool resolveThingCardDevice(AudioContext *ctx,
                                   const char *thingId,
                                   bool isCapture,
                                   uint32_t *card,
                                   uint32_t *device) {
    if (!ctx || !thingId || !card || !device) {
        return false;
    }

    if (isCapture) {
        if (parseCaptureThing(thingId, card, device)) {
            return true;
        }
    } else {
        if (parsePlaybackThing(thingId, card, device)) {
            return true;
        }
    }

    char stableCardId[64] = {};
    if (!parseStableThing(thingId, stableCardId, sizeof(stableCardId), device)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(ctx->devicesMutex);
    for (const auto &entry : ctx->devices) {
        const AudioDeviceInfo &info = entry.second;
        if (strcmp(info.stableCardId, stableCardId) != 0) {
            continue;
        }
        if (info.deviceIndex != *device) {
            continue;
        }
        if (isCapture && !info.hasCapture) {
            continue;
        }
        if (!isCapture && !info.hasPlayback) {
            continue;
        }
        *card = info.cardIndex;
        *device = info.deviceIndex;
        return true;
    }

    return false;
}

static void captureRingReset(RuntimeGraph::AlsaCaptureStream *s) {
    if (!s) {
        return;
    }
    s->ringHead = 0;
    s->ringTail = 0;
    s->ringCount = 0;
    s->readFrac = 0.0f;
    memset(s->lastSample, 0, sizeof(s->lastSample));
}

static void captureRingPush(RuntimeGraph::AlsaCaptureStream *s, const int16_t *in, uint32_t frames) {
    if (!s || !in || s->channels == 0 || s->channels > kMaxChannelsPerThing) {
        return;
    }

    for (uint32_t f = 0; f < frames; ++f) {
        if (s->ringCount >= kCaptureRingFrames) {
            s->ringTail = (s->ringTail + 1U) % kCaptureRingFrames;
            s->ringCount = kCaptureRingFrames - 1U;
        }

        int16_t *dst = &s->ring[s->ringHead * kMaxChannelsPerThing];
        const int16_t *src = &in[f * s->channels];
        for (uint8_t ch = 0; ch < s->channels; ++ch) {
            dst[ch] = src[ch];
        }
        for (uint8_t ch = s->channels; ch < kMaxChannelsPerThing; ++ch) {
            dst[ch] = 0;
        }

        s->ringHead = (s->ringHead + 1U) % kCaptureRingFrames;
        ++s->ringCount;
    }
}

static inline int16_t captureRingSample(const RuntimeGraph::AlsaCaptureStream &s,
                                        uint32_t frameIndex,
                                        uint8_t channel) {
    return s.ring[frameIndex * kMaxChannelsPerThing + channel];
}

static void closeCaptureStream(RuntimeGraph::AlsaCaptureStream *s) {
    if (!s) {
        return;
    }
    if (s->pcm) {
        snd_pcm_close(s->pcm);
        s->pcm = nullptr;
    }
}

static void closePlaybackStream(RuntimeGraph::AlsaPlaybackStream *s) {
    if (!s) {
        return;
    }
    if (s->pcm) {
        snd_pcm_close(s->pcm);
        s->pcm = nullptr;
    }
}

static bool openCaptureStream(RuntimeGraph::AlsaCaptureStream *s) {
    if (!s || !s->active) {
        return false;
    }

    static const struct {
        unsigned periodFrames;
        unsigned periods;
        int configureTiming;
    } profiles[] = {
        {512U, 4U, 1},
        {256U, 4U, 1},
        {BUFFER_FRAMES, 4U, 1},
        {BUFFER_FRAMES, 2U, 1},
        {BUFFER_FRAMES, 2U, 0},
    };

    const unsigned rates[] = {SAMPLE_RATE, AUDIO_INPUT_FALLBACK_RATE};
    uint8_t chAttempts[3] = {};
    size_t chCount = 0;

    if (s->channels > 0 && s->channels <= kMaxChannelsPerThing) {
        chAttempts[chCount++] = s->channels;
    }
    if (chCount == 0 || chAttempts[0] != 2) {
        chAttempts[chCount++] = 2;
    }
    if (chAttempts[0] != 1 && (chCount < 2 || chAttempts[1] != 1)) {
        chAttempts[chCount++] = 1;
    }

    int lastErr = 0;
    const char *lastAttemptPath = s->path;
    snd_pcm_format_t lastAttemptFormat = SND_PCM_FORMAT_UNKNOWN;
    static const snd_pcm_format_t formats[] = {
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S32_LE,
    };

    for (size_t r = 0; r < (sizeof(rates) / sizeof(rates[0])); ++r) {
        for (size_t c = 0; c < chCount; ++c) {
            uint8_t ch = chAttempts[c];
            if (ch == 0 || ch > kMaxChannelsPerThing) {
                continue;
            }

            for (size_t f = 0; f < (sizeof(formats) / sizeof(formats[0])); ++f) {
                for (size_t p = 0; p < (sizeof(profiles) / sizeof(profiles[0])); ++p) {
                    snd_pcm_t *pcm = nullptr;
                    snd_pcm_uframes_t period = 0;
                    size_t frameBytes = 0;
                    snd_pcm_access_t access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
                    int openRc = audio_pcm_open_configured(&pcm,
                                                           s->path,
                                                           SND_PCM_STREAM_CAPTURE,
                                                           rates[r],
                                                           ch,
                                                           formats[f],
                                                           &access,
                                                           profiles[p].periodFrames,
                                                           profiles[p].periods,
                                                           &period,
                                                           &frameBytes,
                                                           profiles[p].configureTiming,
                                                           0);
                    if (openRc != 0 || !pcm) {
                        lastErr = openRc;
                        lastAttemptPath = s->path;
                        lastAttemptFormat = formats[f];
                        continue;
                    }

                    s->pcm = pcm;
                    s->format = formats[f];
                    s->channels = ch;
                    s->sampleRate = rates[r];
                    s->mmapAccess = (access == SND_PCM_ACCESS_MMAP_INTERLEAVED);
                    s->reopenRetryBlocks = 0;
                    captureRingReset(s);
                    float baseRatio = (float)s->sampleRate / (float)SAMPLE_RATE;
                    initAdaptiveSrc(&s->src, 0.50f, baseRatio);
                    if (snd_pcm_start(s->pcm) < 0) {
                        // Capture may auto-start on first read depending on driver.
                    }
                    // VERBOSE: Uncomment this for debugging capture stream open issues.
                    // printf("[AUDIO] [INFO] capture stream opened %s (%uch, %u Hz, fmt=%s period=%u periods=%u)\n",
                    //        s->path,
                    //        (unsigned)s->channels,
                    //        (unsigned)s->sampleRate,
                    //        snd_pcm_format_name(s->format),
                    //        profiles[p].periodFrames,
                    //        profiles[p].periods);
                    return true;
                }
            }
        }
    }

    printf("[AUDIO] [WARN] capture stream open failed: %s fmt=%s (%s)\n",
           lastAttemptPath ? lastAttemptPath : s->path,
           snd_pcm_format_name(lastAttemptFormat),
           snd_strerror(lastErr));
    return false;
}

static bool openPlaybackStream(RuntimeGraph::AlsaPlaybackStream *s) {
    if (!s || !s->active) {
        return false;
    }

    static const struct {
        unsigned periodFrames;
        unsigned periods;
        int configureTiming;
    } profiles[] = {
        {512U, 4U, 1},
        {256U, 4U, 1},
        {BUFFER_FRAMES, 4U, 1},
        {BUFFER_FRAMES, 2U, 1},
        {BUFFER_FRAMES, 2U, 0},
    };

    const unsigned rates[] = {SAMPLE_RATE, AUDIO_INPUT_FALLBACK_RATE};
    uint8_t chAttempts[4] = {};
    size_t chCount = 0;
    if (s->channels > 0 && s->channels <= kMaxChannelsPerThing) {
        chAttempts[chCount++] = s->channels;
    }
    if (chCount == 0 || chAttempts[0] != 2) {
        chAttempts[chCount++] = 2;
    }
    if (chAttempts[0] != 1 && (chCount < 2 || chAttempts[1] != 1)) {
        chAttempts[chCount++] = 1;
    }

    int lastErr = 0;
    const char *lastAttemptPath = s->path;
    snd_pcm_format_t lastAttemptFormat = SND_PCM_FORMAT_UNKNOWN;
    static const snd_pcm_format_t formats[] = {
        SND_PCM_FORMAT_S16_LE,
        SND_PCM_FORMAT_S32_LE,
    };

    for (size_t r = 0; r < (sizeof(rates) / sizeof(rates[0])); ++r) {
        for (size_t c = 0; c < chCount; ++c) {
            uint8_t ch = chAttempts[c];
            if (ch == 0 || ch > kMaxChannelsPerThing) {
                continue;
            }

            for (size_t f = 0; f < (sizeof(formats) / sizeof(formats[0])); ++f) {
                for (size_t p = 0; p < (sizeof(profiles) / sizeof(profiles[0])); ++p) {
                    snd_pcm_t *pcm = nullptr;
                    snd_pcm_uframes_t period = 0;
                    size_t frameBytes = 0;
                    snd_pcm_access_t access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
                    int openRc = audio_pcm_open_configured(&pcm,
                                                           s->path,
                                                           SND_PCM_STREAM_PLAYBACK,
                                                           rates[r],
                                                           ch,
                                                           formats[f],
                                                           &access,
                                                           profiles[p].periodFrames,
                                                           profiles[p].periods,
                                                           &period,
                                                           &frameBytes,
                                                           profiles[p].configureTiming,
                                                           0);
                    if (openRc != 0 || !pcm) {
                        lastErr = openRc;
                        lastAttemptPath = s->path;
                        lastAttemptFormat = formats[f];
                        continue;
                    }

                    s->pcm = pcm;
                    s->format = formats[f];
                    s->channels = ch;
                    s->sampleRate = rates[r];
                    s->mmapAccess = (access == SND_PCM_ACCESS_MMAP_INTERLEAVED);
                    s->reopenRetryBlocks = 0;
                    s->pendingFrames = 0;
                    s->pendingOffsetFrames = 0;
                    // VERBOSE: Uncomment this for debugging playback stream open issues.
                    // printf("[AUDIO] [INFO] playback stream opened %s (%uch, %u Hz, fmt=%s period=%u periods=%u timing=%d)\n",
                    //        s->path,
                    //        (unsigned)s->channels,
                    //        (unsigned)s->sampleRate,
                    //        snd_pcm_format_name(s->format),
                    //        profiles[p].periodFrames,
                    //        profiles[p].periods,
                    //        profiles[p].configureTiming);
                    return true;
                }
            }
        }
    }

    printf("[AUDIO] [WARN] playback stream open failed: %s fmt=%s (%s)\n",
           lastAttemptPath ? lastAttemptPath : s->path,
           snd_pcm_format_name(lastAttemptFormat),
           snd_strerror(lastErr));
    return false;
}

static void maybeReopenCapture(RuntimeGraph::AlsaCaptureStream *s) {
    if (!s || !s->active || s->pcm) {
        return;
    }
    if (s->reopenRetryBlocks > 0) {
        --s->reopenRetryBlocks;
        return;
    }
    if (!openCaptureStream(s)) {
        s->reopenRetryBlocks = kReopenRetryBlocks;
    }
}

static void maybeReopenPlayback(RuntimeGraph::AlsaPlaybackStream *s) {
    if (!s || !s->active || s->pcm) {
        return;
    }
    if (s->reopenRetryBlocks > 0) {
        --s->reopenRetryBlocks;
        return;
    }
    if (!openPlaybackStream(s)) {
        s->reopenRetryBlocks = kReopenRetryBlocks;
    }
}

static const char *mmapSampleAddress(const snd_pcm_channel_area_t *area,
                                    snd_pcm_uframes_t offset,
                                    snd_pcm_uframes_t frame) {
    uint64_t bitOffset = (uint64_t)area->first +
                         (uint64_t)area->step * ((uint64_t)offset + frame);
    return static_cast<const char *>(area->addr) + (bitOffset / 8U);
}

static void captureRingAdvance(RuntimeGraph::AlsaCaptureStream *s, uint32_t frames) {
    for (uint32_t frame = 0; frame < frames; ++frame) {
        if (s->ringCount >= kCaptureRingFrames) {
            s->ringTail = (s->ringTail + 1U) % kCaptureRingFrames;
            s->ringCount = kCaptureRingFrames - 1U;
        }
        s->ringHead = (s->ringHead + 1U) % kCaptureRingFrames;
        ++s->ringCount;
    }
}

static snd_pcm_sframes_t captureMmapRead(RuntimeGraph::AlsaCaptureStream *s,
                                         snd_pcm_uframes_t requestedFrames) {
    snd_pcm_sframes_t available = snd_pcm_avail_update(s->pcm);
    if (available <= 0) {
        return available;
    }

    const snd_pcm_channel_area_t *areas = nullptr;
    snd_pcm_uframes_t offset = 0;
    snd_pcm_uframes_t frames = requestedFrames;
    int rc = snd_pcm_mmap_begin(s->pcm, &areas, &offset, &frames);
    if (rc < 0) {
        return rc;
    }
    if (frames == 0) {
        return snd_pcm_mmap_commit(s->pcm, offset, 0);
    }
    if (!areas) {
        (void)snd_pcm_mmap_commit(s->pcm, offset, 0);
        return -EIO;
    }
    for (uint8_t channel = 0; channel < s->channels; ++channel) {
        if (!areas[channel].addr) {
            (void)snd_pcm_mmap_commit(s->pcm, offset, 0);
            return -EIO;
        }
    }

    for (uint32_t frame = 0; frame < (uint32_t)frames; ++frame) {
        uint32_t ringFrame = (s->ringHead + frame) % kCaptureRingFrames;
        int16_t *dst = &s->ring[ringFrame * kMaxChannelsPerThing];
        for (uint8_t channel = 0; channel < s->channels; ++channel) {
            const char *src = mmapSampleAddress(&areas[channel], offset, frame);
            if (s->format == SND_PCM_FORMAT_S32_LE) {
                int32_t sample = 0;
                memcpy(&sample, src, sizeof(sample));
                dst[channel] = (int16_t)(sample >> 16);
            } else {
                memcpy(&dst[channel], src, sizeof(dst[channel]));
            }
        }
        for (uint8_t channel = s->channels; channel < kMaxChannelsPerThing; ++channel) {
            dst[channel] = 0;
        }
    }

    snd_pcm_sframes_t committed = snd_pcm_mmap_commit(s->pcm, offset, frames);
    if (committed > 0) {
        captureRingAdvance(s, (uint32_t)committed);
    }
    return committed;
}

static snd_pcm_sframes_t playbackMmapWrite(RuntimeGraph::AlsaPlaybackStream *s,
                                           snd_pcm_uframes_t requestedFrames) {
    snd_pcm_sframes_t available = snd_pcm_avail_update(s->pcm);
    if (available <= 0) {
        return available;
    }

    const snd_pcm_channel_area_t *areas = nullptr;
    snd_pcm_uframes_t offset = 0;
    snd_pcm_uframes_t frames = requestedFrames;
    int rc = snd_pcm_mmap_begin(s->pcm, &areas, &offset, &frames);
    if (rc < 0) {
        return rc;
    }
    if (frames == 0) {
        return snd_pcm_mmap_commit(s->pcm, offset, 0);
    }
    if (!areas) {
        (void)snd_pcm_mmap_commit(s->pcm, offset, 0);
        return -EIO;
    }
    for (uint8_t channel = 0; channel < s->channels; ++channel) {
        if (!areas[channel].addr) {
            (void)snd_pcm_mmap_commit(s->pcm, offset, 0);
            return -EIO;
        }
    }

    for (uint32_t frame = 0; frame < (uint32_t)frames; ++frame) {
        uint32_t srcFrame = s->pendingOffsetFrames + frame;
        for (uint8_t channel = 0; channel < s->channels; ++channel) {
            char *dst = const_cast<char *>(mmapSampleAddress(&areas[channel], offset, frame));
            if (s->format == SND_PCM_FORMAT_S32_LE) {
                const int32_t *src = &s->pendingBlock32[srcFrame * s->channels + channel];
                memcpy(dst, src, sizeof(*src));
            } else {
                const int16_t *src = &s->pendingBlock[srcFrame * s->channels + channel];
                memcpy(dst, src, sizeof(*src));
            }
        }
    }

    return snd_pcm_mmap_commit(s->pcm, offset, frames);
}

static void serviceCaptureIo(RuntimeGraph *rt) {
    if (!rt) {
        return;
    }

    for (uint16_t i = 0; i < rt->captureCount; ++i) {
        RuntimeGraph::AlsaCaptureStream &s = rt->capture[i];
        maybeReopenCapture(&s);
        if (!s.pcm) {
            continue;
        }

        while (s.ringCount < kCaptureRingFrames) {
            uint32_t freeFrames = kCaptureRingFrames - s.ringCount;
            uint32_t reqFrames = (freeFrames > BUFFER_FRAMES) ? BUFFER_FRAMES : freeFrames;
            if (reqFrames == 0) {
                break;
            }

            snd_pcm_sframes_t framesRead;
            if (s.mmapAccess) {
                framesRead = captureMmapRead(&s, reqFrames);
            } else {
                void *readBuf = (s.format == SND_PCM_FORMAT_S32_LE)
                                    ? static_cast<void *>(s.ioBlock32)
                                    : static_cast<void *>(s.ioBlock);
                framesRead = snd_pcm_readi(s.pcm, readBuf, reqFrames);
            }

            if (framesRead > 0) {
                if (!s.mmapAccess) {
                    if (s.format == SND_PCM_FORMAT_S32_LE) {
                        int16_t converted[BUFFER_FRAMES * kMaxChannelsPerThing];
                        uint32_t sampleCount = (uint32_t)framesRead * s.channels;
                        for (uint32_t j = 0; j < sampleCount; ++j) {
                            int32_t v32 = s.ioBlock32[j] >> 16;
                            if (v32 > 32767) {
                                v32 = 32767;
                            }
                            if (v32 < -32768) {
                                v32 = -32768;
                            }
                            converted[j] = (int16_t)v32;
                        }
                        captureRingPush(&s, converted, (uint32_t)framesRead);
                    } else {
                        captureRingPush(&s, s.ioBlock, (uint32_t)framesRead);
                    }
                }
                if ((uint32_t)framesRead < reqFrames) {
                    break;
                }
                continue;
            }

            if (framesRead == 0 || framesRead == -EAGAIN) {
                break;
            }

            int err = (framesRead < 0) ? (int)(-framesRead) : EIO;
            if (err == EPIPE || err == ESTRPIPE || err == EIO) {
                if (audio_pcm_recover(s.pcm, -err, s.path, "capture") < 0) {
                    closeCaptureStream(&s);
                    s.reopenRetryBlocks = kReopenRetryBlocks;
                }
                break;
            }

            if (err == ENODEV || err == ENXIO || err == ENOENT || err == EBADFD) {
                closeCaptureStream(&s);
                s.reopenRetryBlocks = kReopenRetryBlocks;
                break;
            }

            break;
        }
    }
}

static void servicePlaybackIo(AudioContext *ctx, RuntimeGraph *rt) {
    if (!ctx || !rt) {
        return;
    }

    for (uint16_t i = 0; i < rt->sinkNodeCount; ++i) {
        const uint16_t node = rt->sinkNodes[i];
        if (strcmp(rt->snapshot.things[node].id, "bluetooth_out") != 0 ||
            !ctx->bluetoothOutputActive.load(std::memory_order_acquire)) {
            continue;
        }
        int16_t block[BUFFER_FRAMES * 2U];
        float gain = ctx->nodeGainAtomics[node].load(std::memory_order_relaxed);
        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            for (uint32_t channel = 0; channel < 2; ++channel) {
                float sample = rt->inputs[node][channel][frame];
                if (!std::isfinite(sample)) {
                    sample = 0.0f;
                }
                sample *= gain * kBluetoothOutputPeakLimit;
                if (sample > 1.0f) sample = 1.0f;
                if (sample < -1.0f) sample = -1.0f;
                int32_t value = (int32_t)lrintf(sample * 32767.0f);
                if (value > 32767) value = 32767;
                if (value < -32768) value = -32768;
                block[frame * 2U + channel] = (int16_t)value;
            }
        }
        (void)ctx->pushBluetoothOutputPcm(block, BUFFER_FRAMES);
    }

    for (uint16_t i = 0; i < rt->playbackCount; ++i) {
        RuntimeGraph::AlsaPlaybackStream &s = rt->playback[i];
        maybeReopenPlayback(&s);
        if (!s.pcm) {
            continue;
        }

        if (s.pendingFrames == 0) {
            const AudioGraphThingInfo &thing = rt->snapshot.things[s.nodeIndex];
            uint8_t inChannels = thing.inputs;
            if (inChannels == 0 || inChannels > kMaxChannelsPerThing) {
                continue;
            }

            for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
                for (uint8_t ch = 0; ch < s.channels; ++ch) {
                    uint8_t srcCh = (ch < inChannels) ? ch : (uint8_t)(inChannels - 1);
                    float v = rt->inputs[s.nodeIndex][srcCh][frame];

                    if (v > 1.0f) {
                        v = 1.0f;
                    }
                    if (v < -1.0f) {
                        v = -1.0f;
                    }
                    int32_t q = (int32_t)lrintf(v * 32767.0f);
                    if (q > 32767) {
                        q = 32767;
                    }
                    if (q < -32768) {
                        q = -32768;
                    }
                    s.pendingBlock[frame * s.channels + ch] = (int16_t)q;
                }
            }

            s.pendingFrames = BUFFER_FRAMES;
            s.pendingOffsetFrames = 0;

            if (s.format == SND_PCM_FORMAT_S32_LE) {
                uint32_t totalSamples = s.pendingFrames * s.channels;
                for (uint32_t i = 0; i < totalSamples; ++i) {
                    s.pendingBlock32[i] = ((int32_t)s.pendingBlock[i]) << 16;
                }
            }
        }

        if (s.pendingFrames <= s.pendingOffsetFrames) {
            s.pendingFrames = 0;
            s.pendingOffsetFrames = 0;
            continue;
        }

        snd_pcm_sframes_t framesWritten;
        if (s.mmapAccess) {
            framesWritten = playbackMmapWrite(&s, s.pendingFrames - s.pendingOffsetFrames);
        } else {
            const void *writeBuf = (s.format == SND_PCM_FORMAT_S32_LE)
                           ? static_cast<const void *>(&s.pendingBlock32[s.pendingOffsetFrames * s.channels])
                           : static_cast<const void *>(&s.pendingBlock[s.pendingOffsetFrames * s.channels]);
            framesWritten = snd_pcm_writei(s.pcm,
                                     writeBuf,
                                     s.pendingFrames - s.pendingOffsetFrames);
        }

        if (framesWritten > 0) {
            if (snd_pcm_state(s.pcm) == SND_PCM_STATE_PREPARED) {
                (void)snd_pcm_start(s.pcm);
            }
            s.pendingOffsetFrames += (uint32_t)framesWritten;
            if (s.pendingOffsetFrames >= s.pendingFrames) {
                s.pendingFrames = 0;
                s.pendingOffsetFrames = 0;
            }
            continue;
        }

        if (framesWritten == 0 || framesWritten == -EAGAIN) {
            continue;
        }

        int err = (framesWritten < 0) ? (int)(-framesWritten) : EIO;
        if (err == EPIPE || err == ESTRPIPE || err == EIO) {
            if (audio_pcm_recover(s.pcm, -err, s.path, "playback") < 0) {
                closePlaybackStream(&s);
                s.reopenRetryBlocks = kReopenRetryBlocks;
            }
            s.pendingFrames = 0;
            s.pendingOffsetFrames = 0;
            continue;
        }

        if (err == ENODEV || err == ENXIO || err == ENOENT || err == EBADFD) {
            closePlaybackStream(&s);
            s.reopenRetryBlocks = kReopenRetryBlocks;
            s.pendingFrames = 0;
            s.pendingOffsetFrames = 0;
            continue;
        }
    }
}

static void closeAllStreams(RuntimeGraph *rt) {
    if (!rt) {
        return;
    }
    for (uint16_t i = 0; i < rt->captureCount; ++i) {
        closeCaptureStream(&rt->capture[i]);
        rt->capture[i].active = 0;
    }
    for (uint16_t i = 0; i < rt->playbackCount; ++i) {
        closePlaybackStream(&rt->playback[i]);
        rt->playback[i].active = 0;
    }
    rt->captureCount = 0;
    rt->playbackCount = 0;
}

static void releaseSfxSlotRef(AudioContext *ctx, uint8_t slotIndex) {
    if (!ctx || slotIndex >= AUDIO_SFX_SLOT_COUNT) {
        return;
    }

    while (1) {
        uint32_t current = ctx->sfxSlotRefs[slotIndex].load(std::memory_order_acquire);
        if (current == 0U) {
            return;
        }
        if (ctx->sfxSlotRefs[slotIndex].compare_exchange_weak(current,
                                                              current - 1U,
                                                              std::memory_order_acq_rel,
                                                              std::memory_order_acquire)) {
            return;
        }
    }
}

static void stopVoice(AudioContext *ctx, RuntimeGraph *rt, uint32_t voiceIndex) {
    if (!ctx || !rt || voiceIndex >= kMaxSoundboardVoices) {
        return;
    }

    RuntimeGraph::SoundboardVoice &voice = rt->soundboardVoices[voiceIndex];
    if (!voice.active) {
        return;
    }

    releaseSfxSlotRef(ctx, voice.slotIndex);
    voice.active = 0;
    voice.hold = 0;
    if (rt->holdVoiceIndex == (int16_t)voiceIndex) {
        rt->holdVoiceIndex = -1;
    }
}

static void stopAllVoices(AudioContext *ctx, RuntimeGraph *rt) {
    if (!ctx || !rt) {
        return;
    }
    for (uint32_t i = 0; i < kMaxSoundboardVoices; ++i) {
        stopVoice(ctx, rt, i);
    }
    rt->holdVoiceIndex = -1;
}

static void drainQueuedSfxEvents(AudioContext *ctx) {
    if (!ctx) {
        return;
    }

    uint32_t read = ctx->sfxQueueRead.load(std::memory_order_relaxed);
    uint32_t write = ctx->sfxQueueWrite.load(std::memory_order_acquire);
    while (read < write) {
        const AudioSfxTriggerEvent &ev = ctx->sfxQueue[read % AUDIO_SFX_QUEUE_CAP];
        releaseSfxSlotRef(ctx, ev.slotIndex);
        ++read;
    }
    ctx->sfxQueueRead.store(read, std::memory_order_release);
}

static int pickVoiceForStart(const RuntimeGraph *rt, bool forHold) {
    if (!rt) {
        return -1;
    }

    for (uint32_t i = 0; i < kMaxSoundboardVoices; ++i) {
        if (!rt->soundboardVoices[i].active) {
            return (int)i;
        }
    }

    int selected = -1;
    uint64_t oldest = UINT64_MAX;
    for (uint32_t i = 0; i < kMaxSoundboardVoices; ++i) {
        const RuntimeGraph::SoundboardVoice &v = rt->soundboardVoices[i];
        if (!v.active) {
            continue;
        }
        if (!forHold && v.hold) {
            continue;
        }
        if (v.startedAtBlock < oldest) {
            oldest = v.startedAtBlock;
            selected = (int)i;
        }
    }

    if (selected >= 0) {
        return selected;
    }

    oldest = UINT64_MAX;
    for (uint32_t i = 0; i < kMaxSoundboardVoices; ++i) {
        const RuntimeGraph::SoundboardVoice &v = rt->soundboardVoices[i];
        if (v.active && v.startedAtBlock < oldest) {
            oldest = v.startedAtBlock;
            selected = (int)i;
        }
    }

    return selected;
}

static void consumeSoundboardTriggers(AudioContext *ctx, RuntimeGraph *rt) {
    if (!ctx || !rt) {
        return;
    }

    uint32_t stopAll = ctx->pendingSfxStopAll.exchange(0U, std::memory_order_acq_rel);
    if (stopAll > 0U) {
        ctx->pendingSfxHoldStops.exchange(0U, std::memory_order_acq_rel);
        stopAllVoices(ctx, rt);
        drainQueuedSfxEvents(ctx);
        ctx->sfxIsPlaying.store(0, std::memory_order_release);
        return;
    }

    uint32_t holdStops = ctx->pendingSfxHoldStops.exchange(0U, std::memory_order_acq_rel);
    if (holdStops > 0U) {
        if (rt->holdVoiceIndex >= 0) {
            stopVoice(ctx, rt, (uint32_t)rt->holdVoiceIndex);
        }
        rt->holdVoiceIndex = -1;
    }

    uint32_t read = ctx->sfxQueueRead.load(std::memory_order_relaxed);
    uint32_t write = ctx->sfxQueueWrite.load(std::memory_order_acquire);
    while (read < write) {
        const AudioSfxTriggerEvent ev = ctx->sfxQueue[read % AUDIO_SFX_QUEUE_CAP];
        ++read;

        if (ev.slotIndex >= AUDIO_SFX_SLOT_COUNT) {
            continue;
        }

        const AudioSfxClipSlot &clip = ctx->sfxSlots[ev.slotIndex];
        if (!clip.loaded ||
            !clip.pcm ||
            clip.frames == 0U ||
            clip.sampleRate == 0U ||
            (clip.channels != 1U && clip.channels != 2U)) {
            releaseSfxSlotRef(ctx, ev.slotIndex);
            continue;
        }

        bool startAsHold = (ev.holdStart != 0U);
        if (startAsHold && rt->holdVoiceIndex >= 0) {
            stopVoice(ctx, rt, (uint32_t)rt->holdVoiceIndex);
        }

        int voiceIndex = pickVoiceForStart(rt, startAsHold);
        if (voiceIndex < 0) {
            releaseSfxSlotRef(ctx, ev.slotIndex);
            continue;
        }

        stopVoice(ctx, rt, (uint32_t)voiceIndex);
        RuntimeGraph::SoundboardVoice &voice = rt->soundboardVoices[(uint32_t)voiceIndex];
        voice.active = 1U;
        voice.hold = startAsHold ? 1U : 0U;
        voice.slotIndex = ev.slotIndex;
        voice.channels = clip.channels;
        voice.frames = clip.frames;
        voice.sampleRate = clip.sampleRate;
        voice.pos = 0U;
        voice.frac = 0.0f;
        voice.pitchRatio = audiox::processing::clampPitchRatio(ev.pitchRatio);
        voice.startedAtBlock = rt->blocksProcessed;

        if (startAsHold) {
            rt->holdVoiceIndex = (int16_t)voiceIndex;
        }
    }

    ctx->sfxQueueRead.store(read, std::memory_order_release);
}

static bool tryReadPublishedGraph(const AudioContext *ctx,
                                  AudioGraphState *out,
                                  uint32_t *seqOut) {
    if (!ctx || !out) {
        return false;
    }

    uint32_t seq1 = ctx->routingGraphSeq.load(std::memory_order_acquire);
    if ((seq1 & 1U) != 0U) {
        return false;
    }

    *out = ctx->routingGraphPublished;

    uint32_t seq2 = ctx->routingGraphSeq.load(std::memory_order_acquire);
    if (seq1 != seq2 || (seq2 & 1U) != 0U) {
        return false;
    }

    if (seqOut) {
        *seqOut = seq2;
    }
    return true;
}

static bool waitForAlsaDeviceReady(AudioContext *ctx, uint64_t timeoutMs) {
    if (!ctx) {
        return false;
    }

    const uint64_t start = monotonicMs();
    uint64_t lastLogMs = 0U;
    uint32_t pollMs = 50U;

    while (true) {
        int rescanRc = ctx->forceRescan();
        {
            std::lock_guard<std::mutex> lock(ctx->devicesMutex);
            if (!ctx->devices.empty()) {
                return true;
            }
        }

        const uint64_t now = monotonicMs();
        if ((now - start) >= timeoutMs) {
            break;
        }

        if (lastLogMs == 0U || (now - lastLogMs) >= 500U) {
            const uint64_t remaining = (timeoutMs > (now - start)) ? (timeoutMs - (now - start)) : 0U;
            printf("[AUDIO] [INFO] waiting for ALSA devices to settle before starting processing thread (%ums remaining)\n",
                   (unsigned)remaining);
            lastLogMs = now;
        }

        if (rescanRc == RET_ERR) {
            break;
        }

        usleep((useconds_t)pollMs * 1000U);
    }

    printf("[AUDIO] [WARN] ALSA device readiness timeout after %ums; continuing with startup without waiting further\n",
           (unsigned)timeoutMs);
    return false;
}

static void configureRealtimeScheduling() {
    sched_param param = {};
    param.sched_priority = 99;
    int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (rc != 0) {
        printf("[AUDIO] [WARN] failed to set SCHED_FIFO priority 99: %s\n", strerror(rc));
        return;
    }

    int policy = 0;
    sched_param active = {};
    if (pthread_getschedparam(pthread_self(), &policy, &active) == 0) {
        printf("[AUDIO] [INFO] processing thread scheduler policy=%d priority=%d\n",
               policy,
               active.sched_priority);
    }
}

static bool thingIdEquals(const AudioGraphThingInfo &thing, const char *id) {
    return id && thing.id[0] && strcmp(thing.id, id) == 0;
}

static bool thingIdEndsWith(const AudioGraphThingInfo &thing, const char *suffix) {
    if (!suffix || !thing.id[0]) {
        return false;
    }
    size_t idLen = strlen(thing.id);
    size_t suffixLen = strlen(suffix);
    if (suffixLen > idLen) {
        return false;
    }
    return strcmp(thing.id + (idLen - suffixLen), suffix) == 0;
}

static int findThingIndex(const AudioGraphState &graph, const char *id) {
    if (!id || !id[0]) {
        return -1;
    }
    for (uint16_t i = 0; i < graph.thingCount; ++i) {
        if (strcmp(graph.things[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static bool sameThingLayout(const AudioGraphState &a, const AudioGraphState &b) {
    if (a.thingCount != b.thingCount) {
        return false;
    }

    for (uint16_t i = 0; i < a.thingCount; ++i) {
        const AudioGraphThingInfo &ta = a.things[i];
        int j = findThingIndex(b, ta.id);
        if (j < 0) {
            return false;
        }

        const AudioGraphThingInfo &tb = b.things[(uint16_t)j];
        if (ta.inputs != tb.inputs || ta.outputs != tb.outputs) {
            return false;
        }
    }

    return true;
}

static bool compileRoute(RuntimeGraph *rt,
                         const AudioGraphEdgeInfo &edge,
                         RuntimeGraph::CompiledRoute *out) {
    if (!rt || !out) {
        return false;
    }

    int srcIndex = findThingIndex(rt->snapshot, edge.src);
    int dstIndex = findThingIndex(rt->snapshot, edge.dst);
    if (srcIndex < 0 || dstIndex < 0) {
        return false;
    }

    const AudioGraphThingInfo &srcThing = rt->snapshot.things[(uint16_t)srcIndex];
    const AudioGraphThingInfo &dstThing = rt->snapshot.things[(uint16_t)dstIndex];
    if (edge.srcChannel >= srcThing.outputs || edge.dstChannel >= dstThing.inputs) {
        return false;
    }
    if (edge.srcChannel >= kMaxChannelsPerThing || edge.dstChannel >= kMaxChannelsPerThing) {
        return false;
    }

    out->srcNode = (uint16_t)srcIndex;
    out->dstNode = (uint16_t)dstIndex;
    out->srcChannel = edge.srcChannel;
    out->dstChannel = edge.dstChannel;
    return true;
}

static uint16_t classifyNode(AudioContext *ctx, const AudioGraphThingInfo &thing) {
    if (thing.outputs == 0) {
        return NODE_SINK;
    }

    if (ctx) {
        audiox::effects::SlotParams fxParams = {};
        if (ctx->getEffectParams(thing.id, &fxParams) == RET_OK) {
            return NODE_EFFECT;
        }
    }

    if (thing.inputs == 0 ||
        thingIdEquals(thing, "soundboard_out") ||
        thingIdEquals(thing, "usb_gadget_in") ||
        thingIdEndsWith(thing, "_in")) {
        return NODE_SOURCE;
    }

    return NODE_PASS;
}

static void clearRuntimeBuffers(RuntimeGraph *rt) {
    if (!rt) {
        return;
    }

    for (uint16_t node = 0; node < rt->snapshot.thingCount; ++node) {
        for (uint8_t ch = 0; ch < kMaxChannelsPerThing; ++ch) {
            rt->inputs[node][ch] = rt->inputStorage[node][ch];
            rt->outputs[node][ch] = rt->outputStorage[node][ch];
            rt->inputContributionCount[node][ch] = 0;
            memset(rt->inputStorage[node][ch], 0, sizeof(rt->inputStorage[node][ch]));
            memset(rt->outputStorage[node][ch], 0, sizeof(rt->outputStorage[node][ch]));
        }
    }
}

static void updateChannelLevels(AudioContext *ctx, RuntimeGraph *rt) {
    if (!ctx || !rt) {
        return;
    }

    for (uint16_t node = 0; node < rt->snapshot.thingCount; ++node) {
        uint8_t channels = rt->snapshot.things[node].outputs;
        if (channels > kMaxChannelsPerThing) {
            channels = kMaxChannelsPerThing;
        }

        if (channels == 0) {
            channels = rt->snapshot.things[node].inputs;
            if (channels > kMaxChannelsPerThing) {
                channels = kMaxChannelsPerThing;
            }
            for (uint8_t ch = 0; ch < channels; ++ch) {
                float peak = 0.0f;
                for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
                    float sample = fabsf(rt->inputs[node][ch][frame]);
                    if (sample > peak) {
                        peak = sample;
                    }
                }
                rt->channelLevels[node][ch].store(peak, std::memory_order_relaxed);
                ctx->nodeChannelLevels[node][ch].store(peak, std::memory_order_relaxed);
            }
        } else {
            for (uint8_t ch = 0; ch < channels; ++ch) {
                float peak = 0.0f;
                for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
                    float sample = fabsf(rt->outputs[node][ch][frame]);
                    if (sample > peak) {
                        peak = sample;
                    }
                }
                rt->channelLevels[node][ch].store(peak, std::memory_order_relaxed);
                ctx->nodeChannelLevels[node][ch].store(peak, std::memory_order_relaxed);
            }
        }
    }
}

static void renderSourceNode(AudioContext *ctx, RuntimeGraph *rt, uint16_t nodeIndex) {
    if (!ctx || !rt || nodeIndex >= rt->snapshot.thingCount) {
        return;
    }

    const AudioGraphThingInfo &thing = rt->snapshot.things[nodeIndex];
    uint8_t outChannels = (thing.outputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.outputs;
    if (outChannels == 0) {
        return;
    }

    if (thingIdEquals(thing, "bluetooth_in")) {
        uint32_t readIndex = ctx->bluetoothPcmRead.load(std::memory_order_relaxed);
        const uint32_t writeIndex = ctx->bluetoothPcmWrite.load(std::memory_order_acquire);
        const uint32_t available = writeIndex - readIndex;
        const float fillRatio = (float)available / (float)AUDIO_BT_PCM_RING_FRAMES;
        const float error = fillRatio - 0.5f;
        ctx->bluetoothPcmRateIntegral += error * 0.00002f;
        if (ctx->bluetoothPcmRateIntegral > 0.0005f) ctx->bluetoothPcmRateIntegral = 0.0005f;
        if (ctx->bluetoothPcmRateIntegral < -0.0005f) ctx->bluetoothPcmRateIntegral = -0.0005f;
        ctx->bluetoothPcmReadRatio = 1.0f + error * 0.001f + ctx->bluetoothPcmRateIntegral;

        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            const uint32_t consumed = (uint32_t)ctx->bluetoothPcmReadFrac;
            const uint32_t indexA = readIndex + consumed;
            const uint32_t indexB = indexA + 1U;
            const bool haveA = (uint32_t)(writeIndex - indexA) > 0;
            const bool haveB = (uint32_t)(writeIndex - indexB) > 0;
            for (uint8_t channel = 0; channel < outChannels; ++channel) {
                float sample = 0.0f;
                if (haveA) {
                    const uint8_t inputChannel = channel < 2 ? channel : 1;
                    const int16_t a = ctx->bluetoothPcmRing[indexA % AUDIO_BT_PCM_RING_FRAMES][inputChannel];
                    const int16_t b = haveB
                        ? ctx->bluetoothPcmRing[indexB % AUDIO_BT_PCM_RING_FRAMES][inputChannel]
                        : a;
                    sample = ((float)a + ((float)b - (float)a) * ctx->bluetoothPcmReadFrac) / 32768.0f;
                }
                rt->outputs[nodeIndex][channel][frame] = sample;
            }
            if (haveA) {
                ctx->bluetoothPcmReadFrac += ctx->bluetoothPcmReadRatio;
                const uint32_t advance = (uint32_t)ctx->bluetoothPcmReadFrac;
                readIndex += advance;
                ctx->bluetoothPcmReadFrac -= (float)advance;
            } else {
                ctx->bluetoothPcmReadFrac = 0.0f;
            }
        }
        ctx->bluetoothPcmRead.store(readIndex, std::memory_order_release);
        return;
    }

    int16_t captureIdx = rt->nodeToCaptureStream[nodeIndex];
    if (captureIdx >= 0 && (uint16_t)captureIdx < rt->captureCount) {
        RuntimeGraph::AlsaCaptureStream &s = rt->capture[(uint16_t)captureIdx];
        float fillRatio = (float)s.ringCount / (float)kCaptureRingFrames;
        float srcRatio = adaptiveSrcStep(&s.src, fillRatio);

        // Check if this is a gadget device to apply gain
        uint8_t isGadgetSource = 0;
        if (ctx) {
            for (const auto &dev : ctx->devices) {
                if (dev.second.cardIndex == s.card && dev.second.deviceIndex == s.device && dev.second.hasCapture) {
                    isGadgetSource = dev.second.isGadget;
                    break;
                }
            }
        }
        float gainMultiplier = isGadgetSource ? USB_GADGET_IN_GAIN : 1.0f;

        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            if (s.ringCount == 0) {
                for (uint8_t ch = 0; ch < outChannels; ++ch) {
                    uint8_t srcCh = (ch < s.channels) ? ch : 0;
                    float out = s.lastSample[srcCh] * gainMultiplier;
                    if (out > 1.0f) out = 1.0f;
                    if (out < -1.0f) out = -1.0f;
                    rt->outputs[nodeIndex][ch][frame] = out;
                }
            } else {
                uint32_t aFrame = s.ringTail;
                uint32_t bFrame = (s.ringCount > 1) ? ((s.ringTail + 1U) % kCaptureRingFrames) : s.ringTail;

                for (uint8_t ch = 0; ch < outChannels; ++ch) {
                    uint8_t srcCh = (ch < s.channels) ? ch : (uint8_t)(s.channels - 1U);
                    float a = (float)captureRingSample(s, aFrame, srcCh) / 32768.0f;
                    float b = (float)captureRingSample(s, bFrame, srcCh) / 32768.0f;
                    float out = (a + ((b - a) * s.readFrac)) * gainMultiplier;
                    if (out > 1.0f) out = 1.0f;
                    if (out < -1.0f) out = -1.0f;
                    rt->outputs[nodeIndex][ch][frame] = out;
                    s.lastSample[srcCh] = out;
                }

                s.readFrac += srcRatio;
                while (s.readFrac >= 1.0f && s.ringCount > 0) {
                    s.ringTail = (s.ringTail + 1U) % kCaptureRingFrames;
                    --s.ringCount;
                    s.readFrac -= 1.0f;
                }
                if (s.ringCount == 0) {
                    s.readFrac = 0.0f;
                }
            }
        }
        return;
    }

    if (thingIdEquals(thing, "soundboard_out")) {
        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            float s = 0.0f;

            for (uint32_t voiceIndex = 0; voiceIndex < kMaxSoundboardVoices; ++voiceIndex) {
                RuntimeGraph::SoundboardVoice &voice = rt->soundboardVoices[voiceIndex];
                if (!voice.active) {
                    continue;
                }

                if (voice.slotIndex >= AUDIO_SFX_SLOT_COUNT ||
                    voice.frames == 0U ||
                    voice.sampleRate == 0U ||
                    (voice.channels != 1U && voice.channels != 2U) ||
                    voice.pos >= voice.frames) {
                    stopVoice(ctx, rt, voiceIndex);
                    continue;
                }

                const AudioSfxClipSlot &clip = ctx->sfxSlots[voice.slotIndex];
                if (!clip.loaded || !clip.pcm) {
                    stopVoice(ctx, rt, voiceIndex);
                    continue;
                }
                uint32_t posA = voice.pos;
                uint32_t posB = (posA + 1U < voice.frames) ? (posA + 1U) : posA;
                float a = (float)clip.pcm[posA * voice.channels] / 32768.0f;
                float b = (float)clip.pcm[posB * voice.channels] / 32768.0f;
                s += (a + ((b - a) * voice.frac)) * kSoundboardClipGain;

                float clipStep = audiox::processing::computePlaybackStep(voice.sampleRate,
                                                                         SAMPLE_RATE,
                                                                         voice.pitchRatio);
                voice.frac += clipStep;
                while (voice.frac >= 1.0f && voice.pos < voice.frames) {
                    ++voice.pos;
                    voice.frac -= 1.0f;
                }

                if (voice.pos >= voice.frames) {
                    stopVoice(ctx, rt, voiceIndex);
                }
            }

            if (s > 0.98f) {
                s = 0.98f;
            }
            if (s < -0.98f) {
                s = -0.98f;
            }

            for (uint8_t ch = 0; ch < outChannels; ++ch) {
                rt->outputs[nodeIndex][ch][frame] = s;
            }
        }
        return;
    }

    // Placeholder for live capture sources. They currently produce silence.
    for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
        for (uint8_t ch = 0; ch < outChannels; ++ch) {
            rt->outputs[nodeIndex][ch][frame] = 0.0f;
        }
    }
}

static void routeCompiledEdge(AudioContext *ctx, RuntimeGraph *rt, const RuntimeGraph::CompiledRoute &route) {
    if (!rt) {
        return;
    }

    float gain = ctx ? ctx->nodeGainAtomics[route.srcNode].load(std::memory_order_relaxed) : 1.0f;
    const float *src = rt->outputs[route.srcNode][route.srcChannel];
    float *dstStorage = rt->inputStorage[route.dstNode][route.dstChannel];
    float *dst = rt->inputs[route.dstNode][route.dstChannel];
    uint8_t &contribCount = rt->inputContributionCount[route.dstNode][route.dstChannel];

    if (contribCount == 0U) {
        if (gain == 1.0f) {
            rt->inputs[route.dstNode][route.dstChannel] = const_cast<float *>(src);
            rt->inputContributionCount[route.dstNode][route.dstChannel] = 1U;
            return;
        }

        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            dstStorage[frame] = src[frame] * gain;
        }
        rt->inputs[route.dstNode][route.dstChannel] = dstStorage;
        rt->inputContributionCount[route.dstNode][route.dstChannel] = 1U;
        return;
    }

    if (dst != dstStorage) {
        memcpy(dstStorage, dst, sizeof(float) * BUFFER_FRAMES);
        dst = dstStorage;
        rt->inputs[route.dstNode][route.dstChannel] = dstStorage;
    }

    if (gain == 1.0f) {
        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            dst[frame] += src[frame];
        }
    } else {
        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            dst[frame] += src[frame] * gain;
        }
    }

    if (contribCount < 255U) {
        ++contribCount;
    }
}

static void copyWithClamp(const float *src, float *dst) {
    for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
        float s = src[frame];
        if (s > 1.0f) {
            s = 1.0f;
        }
        if (s < -1.0f) {
            s = -1.0f;
        }
        dst[frame] = s;
    }
}

static void processNode(AudioContext *ctx, RuntimeGraph *rt, uint16_t nodeIndex, uint32_t effectSeq) {
    if (!ctx || !rt || nodeIndex >= rt->snapshot.thingCount) {
        return;
    }

    const AudioGraphThingInfo &thing = rt->snapshot.things[nodeIndex];
    uint8_t inChannels = (thing.inputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.inputs;
    uint8_t outChannels = (thing.outputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.outputs;
    uint8_t copyChannels = (inChannels < outChannels) ? inChannels : outChannels;

    if (copyChannels == 0 || outChannels == 0) {
        return;
    }

    if (rt->nodeKind[nodeIndex] == NODE_EFFECT) {
        if (rt->effectParamsSeq[nodeIndex] != effectSeq) {
            audiox::effects::SlotParams params = {};
            if (ctx->getEffectParams(thing.id, &params) != RET_OK) {
                params.enabled = 1U;
                params.type = audiox::effects::EFFECT_GAIN;
                audiox::effects::setSlotDefaultsForType(&params, params.type);
            }
            rt->effectParamsCache[nodeIndex] = params;
            rt->effectParamsSeq[nodeIndex] = effectSeq;
        }

        const audiox::effects::SlotParams &params = rt->effectParamsCache[nodeIndex];

        if (!params.enabled) {
            for (uint8_t ch = 0; ch < copyChannels; ++ch) {
                copyWithClamp(rt->inputs[nodeIndex][ch], rt->outputs[nodeIndex][ch]);
            }
            for (uint8_t ch = copyChannels; ch < outChannels; ++ch) {
                memset(rt->outputs[nodeIndex][ch], 0, sizeof(float) * BUFFER_FRAMES);
            }
            return;
        }

        for (uint8_t ch = 0; ch < copyChannels; ++ch) {
            audiox::effects::processSlot(thing.id,
                                         ch,
                                         params,
                                         rt->inputs[nodeIndex][ch],
                                         rt->outputs[nodeIndex][ch],
                                         BUFFER_FRAMES);
        }

        for (uint8_t ch = copyChannels; ch < outChannels; ++ch) {
            memset(rt->outputs[nodeIndex][ch], 0, sizeof(float) * BUFFER_FRAMES);
        }
        return;
    }

    for (uint8_t ch = 0; ch < copyChannels; ++ch) {
        copyWithClamp(rt->inputs[nodeIndex][ch], rt->outputs[nodeIndex][ch]);
    }
}

static void buildProcessRouteIndex(RuntimeGraph *rt) {
    if (!rt) {
        return;
    }

    uint16_t nodeCount = rt->snapshot.thingCount;
    if (nodeCount > AUDIO_GRAPH_MAX_THINGS) {
        nodeCount = AUDIO_GRAPH_MAX_THINGS;
    }

    for (uint16_t node = 0; node < nodeCount; ++node) {
        rt->processRouteStart[node] = 0;
        rt->processRouteLen[node] = 0;
    }

    for (uint16_t i = 0; i < rt->processRouteCount; ++i) {
        const RuntimeGraph::CompiledRoute &route = rt->processRoutes[i];
        if (route.srcNode < nodeCount && rt->processRouteLen[route.srcNode] < AUDIO_GRAPH_MAX_EDGES) {
            ++rt->processRouteLen[route.srcNode];
        }
    }

    uint16_t cursor[AUDIO_GRAPH_MAX_THINGS] = {};
    uint16_t offset = 0;
    for (uint16_t node = 0; node < nodeCount; ++node) {
        rt->processRouteStart[node] = offset;
        cursor[node] = offset;
        offset += rt->processRouteLen[node];
    }

    for (uint16_t i = 0; i < rt->processRouteCount; ++i) {
        const RuntimeGraph::CompiledRoute &route = rt->processRoutes[i];
        if (route.srcNode >= nodeCount) {
            continue;
        }
        uint16_t &write = cursor[route.srcNode];
        if (write < AUDIO_GRAPH_MAX_EDGES) {
            rt->processRoutesBySrc[write++] = route;
        }
    }
}

static void publishPlayingSfxSet(AudioContext *ctx, RuntimeGraph *rt) {
    if (!ctx || !rt) {
        return;
    }

    char names[AUDIO_SFX_SLOT_COUNT][MIDI_SFX_PATH_MAX];
    uint32_t count = 0;
    memset(names, 0, sizeof(names));

    for (uint32_t i = 0; i < kMaxSoundboardVoices && count < AUDIO_SFX_SLOT_COUNT; ++i) {
        const RuntimeGraph::SoundboardVoice &voice = rt->soundboardVoices[i];
        if (!voice.active || voice.hold || voice.slotIndex >= AUDIO_SFX_SLOT_COUNT) {
            continue;
        }

        const AudioSfxClipSlot &slot = ctx->sfxSlots[voice.slotIndex];
        if (!slot.loaded || !slot.name[0]) {
            continue;
        }

        bool exists = false;
        for (uint32_t j = 0; j < count; ++j) {
            if (strcmp(names[j], slot.name) == 0) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }

        size_t n = strnlen(slot.name, MIDI_SFX_PATH_MAX - 1);
        memcpy(names[count], slot.name, n);
        names[count][n] = '\0';
        ++count;
    }

    bool changed = (count != rt->publishedPlayingCount);
    if (!changed) {
        for (uint32_t i = 0; i < count; ++i) {
            if (strcmp(rt->publishedPlayingBasenames[i], names[i]) != 0) {
                changed = true;
                break;
            }
        }
    }

    if (!changed) {
        return;
    }

    rt->publishedPlayingCount = count;
    memset(rt->publishedPlayingBasenames, 0, sizeof(rt->publishedPlayingBasenames));
    for (uint32_t i = 0; i < count; ++i) {
        size_t n = strnlen(names[i], MIDI_SFX_PATH_MAX - 1);
        memcpy(rt->publishedPlayingBasenames[i], names[i], n);
        rt->publishedPlayingBasenames[i][n] = '\0';
    }

    {
        std::lock_guard<std::mutex> lock(ctx->sfxPlayingMutex);
        ctx->sfxPlayingCount = count;
        memset(ctx->sfxPlayingBasenames, 0, sizeof(ctx->sfxPlayingBasenames));
        for (uint32_t i = 0; i < count; ++i) {
            size_t n = strnlen(names[i], MIDI_SFX_PATH_MAX - 1);
            memcpy(ctx->sfxPlayingBasenames[i], names[i], n);
            ctx->sfxPlayingBasenames[i][n] = '\0';
        }
    }
    ctx->sfxPlayingSeq.fetch_add(1U, std::memory_order_release);
}

static void rebuildRuntimeGraph(AudioContext *ctx, RuntimeGraph *rt, const AudioGraphState &graph) {
    if (!ctx || !rt) {
        return;
    }

    bool topologyChanged =
        (graph.topologyGeneration != rt->snapshot.topologyGeneration) ||
        !sameThingLayout(graph, rt->snapshot);

    AudioGraphState compiledGraph = graph;
    int helperIndex = -1;
    for (uint16_t i = 0; i < compiledGraph.thingCount; ++i) {
        if (strcmp(compiledGraph.things[i].id, "routing_helper") == 0) {
            helperIndex = (int)i;
            break;
        }
    }
    if (helperIndex >= 0) {
        AudioGraphEdgeInfo compiledEdges[AUDIO_GRAPH_MAX_EDGES] = {};
        uint16_t compiledEdgeCount = 0;
        for (uint16_t i = 0; i < compiledGraph.edgeCount; ++i) {
            const AudioGraphEdgeInfo &edge = compiledGraph.edges[i];
            if (strcmp(edge.src, "routing_helper") != 0 &&
                strcmp(edge.dst, "routing_helper") != 0 &&
                compiledEdgeCount < AUDIO_GRAPH_MAX_EDGES) {
                compiledEdges[compiledEdgeCount++] = edge;
            }
        }
        for (uint16_t inIndex = 0; inIndex < graph.edgeCount; ++inIndex) {
            const AudioGraphEdgeInfo &incoming = graph.edges[inIndex];
            if (strcmp(incoming.dst, "routing_helper") != 0) {
                continue;
            }
            for (uint16_t outIndex = 0; outIndex < graph.edgeCount; ++outIndex) {
                const AudioGraphEdgeInfo &outgoing = graph.edges[outIndex];
                if (strcmp(outgoing.src, "routing_helper") != 0 ||
                    incoming.dstChannel != outgoing.srcChannel ||
                    compiledEdgeCount >= AUDIO_GRAPH_MAX_EDGES) {
                    continue;
                }
                AudioGraphEdgeInfo direct = {};
                memcpy(direct.src, incoming.src, sizeof(direct.src));
                memcpy(direct.dst, outgoing.dst, sizeof(direct.dst));
                direct.srcChannel = incoming.srcChannel;
                direct.dstChannel = outgoing.dstChannel;
                compiledEdges[compiledEdgeCount++] = direct;
            }
        }
        memcpy(compiledGraph.edges, compiledEdges, sizeof(compiledEdges));
        compiledGraph.edgeCount = compiledEdgeCount;
    }

    rt->snapshot = compiledGraph;
    rt->sourceNodeCount = 0;
    rt->processNodeCount = 0;
    rt->sinkNodeCount = 0;
    rt->sourceRouteCount = 0;
    rt->processRouteCount = 0;
    rt->soundboardNodeIndex = -1;
    for (uint16_t i = 0; i < AUDIO_GRAPH_MAX_THINGS; ++i) {
        rt->effectParamsSeq[i] = 0;
        rt->processRouteStart[i] = 0;
        rt->processRouteLen[i] = 0;
    }

    if (topologyChanged) {
        closeAllStreams(rt);
        rt->captureCount = 0;
        rt->playbackCount = 0;

        for (uint16_t i = 0; i < AUDIO_GRAPH_MAX_THINGS; ++i) {
            rt->nodeToCaptureStream[i] = -1;
            rt->nodeToPlaybackStream[i] = -1;
        }
    }

    for (uint16_t i = 0; i < rt->snapshot.thingCount; ++i) {
        rt->nodeKind[i] = classifyNode(ctx, rt->snapshot.things[i]);
        if (thingIdEquals(rt->snapshot.things[i], "soundboard_out")) {
            rt->soundboardNodeIndex = (int16_t)i;
        }
        if (rt->nodeKind[i] == NODE_SOURCE && rt->sourceNodeCount < AUDIO_GRAPH_MAX_THINGS) {
            rt->sourceNodes[rt->sourceNodeCount++] = i;
        }
        if ((rt->nodeKind[i] == NODE_EFFECT || rt->nodeKind[i] == NODE_PASS) &&
            strcmp(rt->snapshot.things[i].id, "routing_helper") != 0 &&
            rt->processNodeCount < AUDIO_GRAPH_MAX_THINGS) {
            rt->processNodes[rt->processNodeCount++] = i;
        }
        if (rt->nodeKind[i] == NODE_SINK && rt->sinkNodeCount < AUDIO_GRAPH_MAX_THINGS) {
            rt->sinkNodes[rt->sinkNodeCount++] = i;
        }
    }

    if (topologyChanged) {
        for (uint16_t i = 0; i < rt->snapshot.thingCount; ++i) {
            const AudioGraphThingInfo &thing = rt->snapshot.things[i];

            if (rt->nodeKind[i] == NODE_SOURCE && rt->captureCount < kMaxCaptureStreams) {
                uint32_t card = 0;
                uint32_t device = 0;
                if (resolveThingCardDevice(ctx, thing.id, true, &card, &device)) {
                    RuntimeGraph::AlsaCaptureStream &s = rt->capture[rt->captureCount];
                    memset(&s, 0, sizeof(s));
                    s.active = 1;
                    s.pcm = nullptr;
                    s.nodeIndex = i;
                    s.card = card;
                    s.device = device;
                    s.sampleRate = SAMPLE_RATE;
                    s.channels = (thing.outputs == 0) ? 1U : ((thing.outputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.outputs);
                    snprintf(s.path, sizeof(s.path), "hw:%u,%u", (unsigned)card, (unsigned)device);
                    initAdaptiveSrc(&s.src, 0.5f, 1.0f);
                    captureRingReset(&s);
                    if (!openCaptureStream(&s)) {
                        s.reopenRetryBlocks = kReopenRetryBlocks;
                    }
                    rt->nodeToCaptureStream[i] = (int16_t)rt->captureCount;
                    ++rt->captureCount;
                }
            }

            if (rt->nodeKind[i] == NODE_SINK && rt->playbackCount < kMaxPlaybackStreams) {
                uint32_t card = 0;
                uint32_t device = 0;
                if (resolveThingCardDevice(ctx, thing.id, false, &card, &device)) {
                    RuntimeGraph::AlsaPlaybackStream &s = rt->playback[rt->playbackCount];
                    memset(&s, 0, sizeof(s));
                    s.active = 1;
                    s.pcm = nullptr;
                    s.nodeIndex = i;
                    s.card = card;
                    s.device = device;
                    s.sampleRate = SAMPLE_RATE;
                    s.channels = (thing.inputs == 0) ? 1U : ((thing.inputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.inputs);
                    snprintf(s.path, sizeof(s.path), "hw:%u,%u", (unsigned)card, (unsigned)device);
                    if (!openPlaybackStream(&s)) {
                        s.reopenRetryBlocks = kReopenRetryBlocks;
                    }
                    rt->nodeToPlaybackStream[i] = (int16_t)rt->playbackCount;
                    ++rt->playbackCount;
                }
            }
        }
    } else {
        for (uint16_t i = 0; i < AUDIO_GRAPH_MAX_THINGS; ++i) {
            rt->nodeToCaptureStream[i] = -1;
            rt->nodeToPlaybackStream[i] = -1;
        }
        for (uint16_t node = 0; node < rt->snapshot.thingCount; ++node) {
            const AudioGraphThingInfo &thing = rt->snapshot.things[node];
            if (rt->nodeKind[node] == NODE_SOURCE) {
                uint32_t card = 0;
                uint32_t device = 0;
                if (resolveThingCardDevice(ctx, thing.id, true, &card, &device)) {
                    for (uint16_t i = 0; i < rt->captureCount; ++i) {
                        RuntimeGraph::AlsaCaptureStream &s = rt->capture[i];
                        if (s.active && s.card == card && s.device == device) {
                            s.nodeIndex = node;
                            rt->nodeToCaptureStream[node] = (int16_t)i;
                            break;
                        }
                    }
                }
            }
            if (rt->nodeKind[node] == NODE_SINK) {
                uint32_t card = 0;
                uint32_t device = 0;
                if (resolveThingCardDevice(ctx, thing.id, false, &card, &device)) {
                    for (uint16_t i = 0; i < rt->playbackCount; ++i) {
                        RuntimeGraph::AlsaPlaybackStream &s = rt->playback[i];
                        if (s.active && s.card == card && s.device == device) {
                            s.nodeIndex = node;
                            rt->nodeToPlaybackStream[node] = (int16_t)i;
                            break;
                        }
                    }
                }
            }
        }
    }

    for (uint16_t edgeIndex = 0; edgeIndex < rt->snapshot.edgeCount; ++edgeIndex) {
        const AudioGraphEdgeInfo &edge = rt->snapshot.edges[edgeIndex];
        RuntimeGraph::CompiledRoute route = {};
        if (!compileRoute(rt, edge, &route)) {
            continue;
        }

        uint16_t srcKind = rt->nodeKind[route.srcNode];
        if (srcKind == NODE_SOURCE && rt->sourceRouteCount < AUDIO_GRAPH_MAX_EDGES) {
            rt->sourceRoutes[rt->sourceRouteCount++] = route;
        } else if ((srcKind == NODE_EFFECT || srcKind == NODE_PASS) &&
                   rt->processRouteCount < AUDIO_GRAPH_MAX_EDGES) {
            rt->processRoutes[rt->processRouteCount++] = route;
        }
    }

    // Topologically sort processNodes so chained effects are always processed
    // in data-flow order regardless of effect ID / thing-list order.
    {
        const uint16_t n = rt->processNodeCount;
        if (n > 1) {
            uint16_t inDegree[AUDIO_GRAPH_MAX_THINGS] = {};
            for (uint16_t r = 0; r < rt->processRouteCount; ++r) {
                const RuntimeGraph::CompiledRoute &route = rt->processRoutes[r];
                bool srcIsProcess = false;
                bool dstIsProcess = false;
                for (uint16_t i = 0; i < n; ++i) {
                    if (rt->processNodes[i] == route.srcNode) srcIsProcess = true;
                    if (rt->processNodes[i] == route.dstNode) dstIsProcess = true;
                }
                if (srcIsProcess && dstIsProcess) {
                    inDegree[route.dstNode]++;
                }
            }

            uint16_t sorted[AUDIO_GRAPH_MAX_THINGS];
            uint16_t sortedCount = 0;
            bool visited[AUDIO_GRAPH_MAX_THINGS] = {};

            for (uint16_t pass = 0; pass < n; ++pass) {
                int picked = -1;
                for (uint16_t i = 0; i < n; ++i) {
                    uint16_t node = rt->processNodes[i];
                    if (!visited[node] && inDegree[node] == 0) {
                        picked = (int)node;
                        visited[node] = true;
                        break;
                    }
                }
                if (picked < 0) {
                    break;
                }
                sorted[sortedCount++] = (uint16_t)picked;
                for (uint16_t r = 0; r < rt->processRouteCount; ++r) {
                    if (rt->processRoutes[r].srcNode == (uint16_t)picked &&
                        inDegree[rt->processRoutes[r].dstNode] > 0) {
                        --inDegree[rt->processRoutes[r].dstNode];
                    }
                }
            }

            if (sortedCount == n) {
                memcpy(rt->processNodes, sorted, n * sizeof(uint16_t));
            }
        }
    }

    buildProcessRouteIndex(rt);

    clearRuntimeBuffers(rt);
}

static void processGraphBlock(AudioContext *ctx, RuntimeGraph *rt) {
    if (!ctx || !rt) {
        return;
    }

    consumeSoundboardTriggers(ctx, rt);
    clearRuntimeBuffers(rt);

    serviceCaptureIo(rt);

    // First pass: render all source nodes.
    for (uint16_t i = 0; i < rt->sourceNodeCount; ++i) {
        renderSourceNode(ctx, rt, rt->sourceNodes[i]);
    }

    // Route source output into downstream node inputs.
    for (uint16_t i = 0; i < rt->sourceRouteCount; ++i) {
        routeCompiledEdge(ctx, rt, rt->sourceRoutes[i]);
    }

    // Process transform/effect nodes in deterministic order and route each
    // node's output forward immediately so chained effects work in one block.
    uint32_t effectSeq = ctx->effectStatesSeq.load(std::memory_order_acquire);
    for (uint16_t i = 0; i < rt->processNodeCount; ++i) {
        uint16_t nodeIndex = rt->processNodes[i];
        processNode(ctx, rt, nodeIndex, effectSeq);

        uint16_t routeStart = rt->processRouteStart[nodeIndex];
        uint16_t routeLen = rt->processRouteLen[nodeIndex];
        for (uint16_t r = 0; r < routeLen; ++r) {
            const RuntimeGraph::CompiledRoute &route = rt->processRoutesBySrc[routeStart + r];
            routeCompiledEdge(ctx, rt, route);
        }
    }

    rt->blocksProcessed++;
    updateChannelLevels(ctx, rt);

    servicePlaybackIo(ctx, rt);

    publishPlayingSfxSet(ctx, rt);

    bool clipActive = false;
    for (uint32_t i = 0; i < kMaxSoundboardVoices; ++i) {
        if (rt->soundboardVoices[i].active) {
            clipActive = true;
            break;
        }
    }

    if (clipActive && !ctx->sfxIsPlaying.load(std::memory_order_relaxed)) {
        ctx->sfxIsPlaying.store(1, std::memory_order_release);
    }
    if (!clipActive && ctx->sfxIsPlaying.load(std::memory_order_relaxed)) {
        ctx->sfxIsPlaying.store(0, std::memory_order_release);
    }
}

static void maybeLogStats(RuntimeGraph *rt) {
    if (!rt) {
        return;
    }

    uint64_t now = monotonicMs();
    if (now < rt->nextStatsBlock) {
        return;
    }

    rt->nextStatsBlock = now + 1000ULL;

    float peak = 0.0f;
    for (uint16_t i = 0; i < rt->sinkNodeCount; ++i) {
        uint16_t node = rt->sinkNodes[i];
        const AudioGraphThingInfo &thing = rt->snapshot.things[node];
        uint8_t channels = (thing.inputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.inputs;
        for (uint8_t ch = 0; ch < channels; ++ch) {
            for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
                float a = fabsf(rt->inputs[node][ch][frame]);
                if (a > peak) {
                    peak = a;
                }
            }
        }
    }

    // VERBOSE
    // printf("[AUDIO] [INFO] graph block=%llu generation=%u sink_peak=%.3f\n",
    //        (unsigned long long)rt->blocksProcessed,
    //        (unsigned)rt->snapshot.generation,
    //        peak);

    for (uint16_t i = 0; i < rt->captureCount; ++i) {
        const RuntimeGraph::AlsaCaptureStream &s = rt->capture[i];
        // VERBOSE
        // printf("[AUDIO] [INFO] capture[%u] ratio=%.5f ring_fill=%u/%u path=%s\n",
        //        (unsigned)i,
        //        s.src.ratio,
        //        (unsigned)s.ringCount,
        //        (unsigned)kCaptureRingFrames,
        //        s.path);
        if (!s.pcm) {
            printf("[AUDIO] [WARN] capture[%u] offline: %s\n", (unsigned)i, s.path);
        }
    }

    for (uint16_t i = 0; i < rt->playbackCount; ++i) {
        const RuntimeGraph::AlsaPlaybackStream &p = rt->playback[i];
        if (!p.pcm) {
            printf("[AUDIO] [WARN] playback[%u] offline: %s\n", (unsigned)i, p.path);
        } else {
            // VERBOSE
            // printf("[AUDIO] [INFO] playback[%u] active: %s (%uch @ %u)\n",
            //        (unsigned)i,
            //        p.path,
            //        (unsigned)p.channels,
            //        (unsigned)p.sampleRate);
        }
    }
}

static void *audioProcessingThreadMain(void *arg) {
    AudioContext *ctx = reinterpret_cast<AudioContext *>(arg);
    if (!ctx) {
        return nullptr;
    }

    RuntimeGraph runtime = {};
    runtime.holdVoiceIndex = -1;
    runtime.effectParamsSeq[0] = 0;
    for (uint16_t i = 1; i < AUDIO_GRAPH_MAX_THINGS; ++i) {
        runtime.effectParamsSeq[i] = 0;
    }
    uint32_t lastGraphGeneration = 0;
    uint32_t lastGraphSeq = 0;
    timespec nextDeadline = monotonicNow();
    addNs(&nextDeadline, blockPeriodNs());

    configureRealtimeScheduling();

    while (ctx->processingThreadRun.load(std::memory_order_acquire)) {
        AudioGraphState graphSnapshot = runtime.snapshot;
        uint32_t readSeq = 0;
        uint32_t seqNow = ctx->routingGraphSeq.load(std::memory_order_relaxed);
        if (seqNow != lastGraphSeq && tryReadPublishedGraph(ctx, &graphSnapshot, &readSeq)) {
            lastGraphSeq = readSeq;
            if (graphSnapshot.generation != lastGraphGeneration) {
                lastGraphGeneration = graphSnapshot.generation;
                if (lastGraphGeneration > 0) {
                    printf("[AUDIO] [INFO] processing thread observed graph generation %u (%u thing(s), %u edge(s))\n",
                           (unsigned)lastGraphGeneration,
                           (unsigned)graphSnapshot.thingCount,
                           (unsigned)graphSnapshot.edgeCount);
                }
            }
        }

        if (graphSnapshot.generation != runtime.snapshot.generation) {
            rebuildRuntimeGraph(ctx, &runtime, graphSnapshot);
            ctx->snapshotGainsForGraph(graphSnapshot);
        }

        if (runtime.snapshot.generation > 0 && runtime.snapshot.thingCount > 0) {
            processGraphBlock(ctx, &runtime);
            maybeLogStats(&runtime);
        }

        waitUntilBlockDeadline(&nextDeadline);
    }

    closeAllStreams(&runtime);

    return nullptr;
}

} // namespace

uint32_t AudioContext::pushBluetoothPcm(const int16_t *stereoFrames, uint32_t frames) {
    if (!stereoFrames || frames == 0) {
        return 0;
    }
    uint32_t writeIndex = bluetoothPcmWrite.load(std::memory_order_relaxed);
    const uint32_t readIndex = bluetoothPcmRead.load(std::memory_order_acquire);
    uint32_t freeFrames = AUDIO_BT_PCM_RING_FRAMES - (writeIndex - readIndex);
    uint32_t accepted = frames < freeFrames ? frames : freeFrames;
    for (uint32_t frame = 0; frame < accepted; ++frame) {
        uint32_t slot = (writeIndex + frame) % AUDIO_BT_PCM_RING_FRAMES;
        bluetoothPcmRing[slot][0] = stereoFrames[frame * 2U];
        bluetoothPcmRing[slot][1] = stereoFrames[frame * 2U + 1U];
    }
    bluetoothPcmWrite.store(writeIndex + accepted, std::memory_order_release);
    if (accepted < frames) {
        bluetoothPcmDropped.fetch_add(frames - accepted, std::memory_order_relaxed);
    }
    return accepted;
}

uint32_t AudioContext::pushBluetoothOutputPcm(const int16_t *stereoFrames, uint32_t frames) {
    if (!stereoFrames || frames == 0 || !bluetoothOutputActive.load(std::memory_order_acquire)) {
        return 0;
    }
    uint32_t writeIndex = bluetoothOutputWrite.load(std::memory_order_relaxed);
    const uint32_t readIndex = bluetoothOutputRead.load(std::memory_order_acquire);
    uint32_t freeFrames = AUDIO_BT_PCM_RING_FRAMES - (writeIndex - readIndex);
    uint32_t accepted = frames < freeFrames ? frames : freeFrames;
    for (uint32_t frame = 0; frame < accepted; ++frame) {
        uint32_t slot = (writeIndex + frame) % AUDIO_BT_PCM_RING_FRAMES;
        bluetoothOutputRing[slot][0] = stereoFrames[frame * 2U];
        bluetoothOutputRing[slot][1] = stereoFrames[frame * 2U + 1U];
    }
    bluetoothOutputWrite.store(writeIndex + accepted, std::memory_order_release);
    if (accepted < frames) {
        bluetoothOutputDropped.fetch_add(frames - accepted, std::memory_order_relaxed);
    }
    return accepted;
}

void AudioContext::updateBluetoothOutputRate(uint32_t packetFrames, uint64_t nowMs) {
    if (packetFrames == 0 || !bluetoothOutputActive.load(std::memory_order_acquire)) {
        return;
    }
    if (bluetoothOutputRateLastUpdateMs != 0 &&
        nowMs - bluetoothOutputRateLastUpdateMs < 10U) {
        return;
    }

    const uint32_t available = getBluetoothOutputAvailable();
    uint64_t elapsedMs = bluetoothOutputRateLastUpdateMs == 0
                             ? 10U
                             : nowMs - bluetoothOutputRateLastUpdateMs;
    if (elapsedMs > 100U) {
        elapsedMs = 100U;
    }
    bluetoothOutputRateLastUpdateMs = nowMs;

    if (!bluetoothOutputRateInitialized) {
        bluetoothOutputFilteredFill = (float)available;
        bluetoothOutputRateInitialized = 1U;
    } else {
        float alpha = 1.0f - expf(-(float)elapsedMs / 80.0f);
        bluetoothOutputFilteredFill +=
            ((float)available - bluetoothOutputFilteredFill) * alpha;
    }

    const float targetFill = (float)packetFrames * 0.5f + (float)BUFFER_FRAMES * 0.5f;
    const float fillError = (bluetoothOutputFilteredFill - targetFill) / targetFill;
    bluetoothOutputRateIntegral += fillError * (float)elapsedMs * 0.0000001175f;
    if (bluetoothOutputRateIntegral > 0.003f) bluetoothOutputRateIntegral = 0.003f;
    if (bluetoothOutputRateIntegral < -0.003f) bluetoothOutputRateIntegral = -0.003f;
    float ratio = 1.0f + fillError * 0.0015f + bluetoothOutputRateIntegral;
    if (ratio > 1.005f) ratio = 1.005f;
    if (ratio < 0.995f) ratio = 0.995f;
    bluetoothOutputRateRatio = ratio;
}

uint32_t AudioContext::resampleBluetoothOutputPcm(int16_t *stereoFrames, uint32_t outputFrames) {
    if (!stereoFrames || outputFrames == 0) {
        return 0;
    }
    uint32_t readIndex = bluetoothOutputRead.load(std::memory_order_relaxed);
    const uint32_t writeIndex = bluetoothOutputWrite.load(std::memory_order_acquire);
    const uint32_t available = writeIndex - readIndex;
    const float ratio = bluetoothOutputRateRatio;

    const uint32_t neededFrames = (uint32_t)(bluetoothOutputReadFraction +
                                             (float)outputFrames * ratio) + 2U;
    if (available < neededFrames) {
        return 0;
    }

    float fraction = bluetoothOutputReadFraction;
    for (uint32_t frame = 0; frame < outputFrames; ++frame) {
        uint32_t advance = (uint32_t)fraction;
        uint32_t slotA = (readIndex + advance) % AUDIO_BT_PCM_RING_FRAMES;
        uint32_t slotB = (slotA + 1U) % AUDIO_BT_PCM_RING_FRAMES;
        for (uint32_t channel = 0; channel < 2; ++channel) {
            float a = bluetoothOutputRing[slotA][channel];
            float b = bluetoothOutputRing[slotB][channel];
            float value = a + (b - a) * (fraction - (float)advance);
            if (value > 32767.0f) value = 32767.0f;
            if (value < -32768.0f) value = -32768.0f;
            stereoFrames[frame * 2U + channel] = (int16_t)value;
        }
        fraction += ratio;
    }
    const uint32_t consumed = (uint32_t)fraction;
    bluetoothOutputReadFraction = fraction - (float)consumed;
    bluetoothOutputRead.store(readIndex + consumed, std::memory_order_release);
    return outputFrames;
}

uint32_t AudioContext::getBluetoothOutputAvailable() const {
    const uint32_t writeIndex = bluetoothOutputWrite.load(std::memory_order_acquire);
    const uint32_t readIndex = bluetoothOutputRead.load(std::memory_order_acquire);
    return writeIndex - readIndex;
}

void AudioContext::setBluetoothOutputActive(bool active) {
    bluetoothOutputActive.store(0, std::memory_order_release);
    const uint32_t writeIndex = bluetoothOutputWrite.load(std::memory_order_acquire);
    bluetoothOutputRead.store(writeIndex, std::memory_order_release);
    bluetoothOutputReadFraction = 0.0f;
    bluetoothOutputRateIntegral = 0.0f;
    bluetoothOutputRateRatio = 1.0f;
    bluetoothOutputFilteredFill = 0.0f;
    bluetoothOutputRateLastUpdateMs = 0;
    bluetoothOutputRateInitialized = 0;
    if (active) {
        bluetoothOutputActive.store(1, std::memory_order_release);
    }
}

int AudioContext::setupThreads() {
    if (processingThreadStarted) {
        return RET_OK;
    }

    const uint64_t alsaReadyTimeoutMs = 2000U;
    (void)waitForAlsaDeviceReady(this, alsaReadyTimeoutMs);

    int initGraphRc = forceRescan();
    if (initGraphRc == RET_ERR) {
        return RET_ERR;
    }

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        printf("[AUDIO] [WARN] failed to lock process memory: %s\n", strerror(errno));
    }

    processingThreadRun.store(1, std::memory_order_release);
    if (pthread_create(&processingThread, nullptr, audioProcessingThreadMain, this) != 0) {
        processingThreadRun.store(0, std::memory_order_release);
        return RET_ERR;
    }

    processingThreadStarted = 1;
    return (initGraphRc == RET_WARN) ? RET_WARN : RET_OK;
}
