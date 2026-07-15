#include "audio/context.hpp"
#include "audio/alsa_pcm.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sched.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr uint32_t kMaxChannelsPerThing = 16;
constexpr float kTwoPi = 6.28318530717958647692f;
constexpr float kSourceToneHz = 220.0f;
constexpr float kSrcRatioMin = 0.97f;
constexpr float kSrcRatioMax = 1.03f;
constexpr float kSrcP = 0.035f;
constexpr float kSrcI = 0.003f;
constexpr uint32_t kMaxCaptureStreams = 4;
constexpr uint32_t kMaxPlaybackStreams = 4;
constexpr uint32_t kCaptureRingFrames = BUFFER_FRAMES * 16U;
constexpr uint32_t kReopenRetryBlocks = 200;
constexpr uint32_t kSoundboardBurstSamples = SAMPLE_RATE / 3U;
constexpr uint32_t kSoundboardBurstMax = SAMPLE_RATE * 3U;
constexpr float kSoundboardClipGain = 0.35f;

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
    struct CompiledRoute {
        uint16_t srcNode;
        uint16_t dstNode;
        uint8_t srcChannel;
        uint8_t dstChannel;
    };

    struct AlsaCaptureStream {
        int active;
        int fd;
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
        AdaptiveSrcController src;
        float lastSample[kMaxChannelsPerThing];
        char path[64];
        int16_t ioBlock[BUFFER_FRAMES * kMaxChannelsPerThing];
        int16_t ring[kCaptureRingFrames * kMaxChannelsPerThing];
    };

    struct AlsaPlaybackStream {
        int active;
        int fd;
        uint16_t nodeIndex;
        uint8_t channels;
        uint32_t card;
        uint32_t device;
        uint32_t sampleRate;
        uint32_t reopenRetryBlocks;
        uint32_t pendingFrames;
        uint32_t pendingOffsetFrames;
        char path[64];
        int16_t pendingBlock[BUFFER_FRAMES * kMaxChannelsPerThing];
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
    uint16_t processRouteCount;
    float inputs[AUDIO_GRAPH_MAX_THINGS][kMaxChannelsPerThing][BUFFER_FRAMES];
    float outputs[AUDIO_GRAPH_MAX_THINGS][kMaxChannelsPerThing][BUFFER_FRAMES];
    float soundboardPhase;
    float soundboardEnv;
    uint32_t soundboardBurstRemaining;
    uint32_t activeClipSlot;
    uint32_t activeClipFrames;
    uint32_t activeClipRate;
    uint8_t activeClipChannels;
    uint32_t activeClipPos;
    float activeClipFrac;
    int16_t nodeToCaptureStream[AUDIO_GRAPH_MAX_THINGS];
    int16_t nodeToPlaybackStream[AUDIO_GRAPH_MAX_THINGS];
    int16_t soundboardNodeIndex;
    AlsaCaptureStream capture[kMaxCaptureStreams];
    AlsaPlaybackStream playback[kMaxPlaybackStreams];
    uint16_t captureCount;
    uint16_t playbackCount;
    uint64_t blocksProcessed;
    uint64_t nextStatsBlock;
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

    (void)clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, deadline, nullptr);
    addNs(deadline, blockPeriodNs());

    timespec now = monotonicNow();
    if (cmpTimespec(now, *deadline) > 0) {
        // If we overran, re-anchor to avoid accumulating wakeup drift.
        *deadline = now;
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
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static void closePlaybackStream(RuntimeGraph::AlsaPlaybackStream *s) {
    if (!s) {
        return;
    }
    if (s->fd >= 0) {
        close(s->fd);
        s->fd = -1;
    }
}

static bool openCaptureStream(RuntimeGraph::AlsaCaptureStream *s) {
    if (!s || !s->active) {
        return false;
    }

    static const struct {
        unsigned periodFrames;
        unsigned periods;
    } profiles[] = {
        {512U, 4U},
        {256U, 4U},
        {BUFFER_FRAMES, 4U},
        {BUFFER_FRAMES, 2U},
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

    for (size_t r = 0; r < (sizeof(rates) / sizeof(rates[0])); ++r) {
        for (size_t c = 0; c < chCount; ++c) {
            uint8_t ch = chAttempts[c];
            if (ch == 0 || ch > kMaxChannelsPerThing) {
                continue;
            }

            for (size_t p = 0; p < (sizeof(profiles) / sizeof(profiles[0])); ++p) {
                int fd = open(s->path, O_RDONLY | O_NONBLOCK);
                if (fd < 0) {
                    return false;
                }

                snd_pcm_uframes_t period = 0;
                size_t frameBytes = 0;
                if (audio_pcm_configure_hw_fd(fd,
                                              s->path,
                                              rates[r],
                                              ch,
                                              profiles[p].periodFrames,
                                              profiles[p].periods,
                                              &period,
                                              &frameBytes,
                                              1,
                                              0) != 0) {
                    close(fd);
                    continue;
                }

                s->fd = fd;
                s->channels = ch;
                s->sampleRate = rates[r];
                s->reopenRetryBlocks = 0;
                captureRingReset(s);
                float baseRatio = (float)s->sampleRate / (float)SAMPLE_RATE;
                initAdaptiveSrc(&s->src, 0.50f, baseRatio);
                printf("[AUDIO] [INFO] capture stream opened %s (%uch, %u Hz, period=%u periods=%u)\n",
                       s->path,
                       (unsigned)s->channels,
                       (unsigned)s->sampleRate,
                       profiles[p].periodFrames,
                       profiles[p].periods);
                return true;
            }
        }
    }

    printf("[AUDIO] [WARN] capture stream open failed: %s\n", s->path);
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

    for (size_t r = 0; r < (sizeof(rates) / sizeof(rates[0])); ++r) {
        for (size_t c = 0; c < chCount; ++c) {
            uint8_t ch = chAttempts[c];
            if (ch == 0 || ch > kMaxChannelsPerThing) {
                continue;
            }

            for (size_t p = 0; p < (sizeof(profiles) / sizeof(profiles[0])); ++p) {
                int fd = open(s->path, O_WRONLY | O_NONBLOCK);
                if (fd < 0) {
                    printf("[AUDIO] [WARN] playback open failed: %s (%s)\n", s->path, strerror(errno));
                    return false;
                }

                snd_pcm_uframes_t period = 0;
                size_t frameBytes = 0;
                if (audio_pcm_configure_hw_fd(fd,
                                              s->path,
                                              rates[r],
                                              ch,
                                              profiles[p].periodFrames,
                                              profiles[p].periods,
                                              &period,
                                              &frameBytes,
                                              profiles[p].configureTiming,
                                              0) != 0) {
                    close(fd);
                    continue;
                }

                s->fd = fd;
                s->channels = ch;
                s->sampleRate = rates[r];
                s->reopenRetryBlocks = 0;
                s->pendingFrames = 0;
                s->pendingOffsetFrames = 0;
                printf("[AUDIO] [INFO] playback stream opened %s (%uch, %u Hz, period=%u periods=%u timing=%d)\n",
                       s->path,
                       (unsigned)s->channels,
                       (unsigned)s->sampleRate,
                       profiles[p].periodFrames,
                       profiles[p].periods,
                       profiles[p].configureTiming);
                return true;
            }
        }
    }

    printf("[AUDIO] [WARN] playback stream open failed all profiles: %s\n", s->path);
    return false;
}

static void maybeReopenCapture(RuntimeGraph::AlsaCaptureStream *s) {
    if (!s || !s->active || s->fd >= 0) {
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
    if (!s || !s->active || s->fd >= 0) {
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

static void serviceCaptureIo(RuntimeGraph *rt) {
    if (!rt) {
        return;
    }

    for (uint16_t i = 0; i < rt->captureCount; ++i) {
        RuntimeGraph::AlsaCaptureStream &s = rt->capture[i];
        maybeReopenCapture(&s);
        if (s.fd < 0) {
            continue;
        }

        while (s.ringCount < kCaptureRingFrames) {
            uint32_t freeFrames = kCaptureRingFrames - s.ringCount;
            uint32_t reqFrames = (freeFrames > BUFFER_FRAMES) ? BUFFER_FRAMES : freeFrames;
            if (reqFrames == 0) {
                break;
            }

            snd_xferi xfer = {};
            xfer.buf = s.ioBlock;
            xfer.frames = reqFrames;

            int rc = ioctl(s.fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
            snd_pcm_sframes_t framesRead = xfer.result;
            if (rc < 0 && framesRead >= 0) {
                framesRead = -errno;
            }

            if (framesRead > 0) {
                captureRingPush(&s, s.ioBlock, (uint32_t)framesRead);
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
                (void)ioctl(s.fd, SNDRV_PCM_IOCTL_PREPARE);
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

static void servicePlaybackIo(RuntimeGraph *rt) {
    if (!rt) {
        return;
    }

    for (uint16_t i = 0; i < rt->playbackCount; ++i) {
        RuntimeGraph::AlsaPlaybackStream &s = rt->playback[i];
        maybeReopenPlayback(&s);
        if (s.fd < 0) {
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
        }

        if (s.pendingFrames <= s.pendingOffsetFrames) {
            s.pendingFrames = 0;
            s.pendingOffsetFrames = 0;
            continue;
        }

        snd_xferi xfer = {};
        xfer.buf = &s.pendingBlock[s.pendingOffsetFrames * s.channels];
        xfer.frames = s.pendingFrames - s.pendingOffsetFrames;

        int rc = ioctl(s.fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
        snd_pcm_sframes_t framesWritten = xfer.result;
        if (rc < 0 && framesWritten >= 0) {
            framesWritten = -errno;
        }

        if (framesWritten > 0) {
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
            (void)ioctl(s.fd, SNDRV_PCM_IOCTL_PREPARE);
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

static void consumeSoundboardTriggers(AudioContext *ctx, RuntimeGraph *rt) {
    if (!ctx || !rt) {
        return;
    }

    uint32_t pending = ctx->pendingSfxTriggers.exchange(0U, std::memory_order_acq_rel);
    if (pending == 0) {
        return;
    }

    uint32_t activeSlot = ctx->sfxActiveSlot.load(std::memory_order_acquire) & 1U;
    const AudioSfxClipSlot &clip = ctx->sfxSlots[activeSlot];
    rt->activeClipSlot = activeSlot;
    rt->activeClipFrames = clip.frames;
    rt->activeClipRate = clip.sampleRate;
    rt->activeClipChannels = clip.channels;
    rt->activeClipPos = 0;
    rt->activeClipFrac = 0.0f;

    uint64_t add = (uint64_t)pending * (uint64_t)kSoundboardBurstSamples;
    uint64_t total = (uint64_t)rt->soundboardBurstRemaining + add;
    if (total > kSoundboardBurstMax) {
        total = kSoundboardBurstMax;
    }
    rt->soundboardBurstRemaining = (uint32_t)total;
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

static bool thingIdStartsWith(const AudioGraphThingInfo &thing, const char *prefix) {
    if (!prefix || !thing.id[0]) {
        return false;
    }
    size_t prefixLen = strlen(prefix);
    return strncmp(thing.id, prefix, prefixLen) == 0;
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

static uint16_t classifyNode(const AudioGraphThingInfo &thing) {
    if (thing.outputs == 0) {
        return NODE_SINK;
    }

    if (thingIdStartsWith(thing, "fx_slot_")) {
        return NODE_EFFECT;
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
            memset(rt->inputs[node][ch], 0, sizeof(rt->inputs[node][ch]));
            memset(rt->outputs[node][ch], 0, sizeof(rt->outputs[node][ch]));
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

static void renderSourceNode(const AudioContext *ctx, RuntimeGraph *rt, uint16_t nodeIndex) {
    if (!ctx || !rt || nodeIndex >= rt->snapshot.thingCount) {
        return;
    }

    const AudioGraphThingInfo &thing = rt->snapshot.things[nodeIndex];
    uint8_t outChannels = (thing.outputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.outputs;
    if (outChannels == 0) {
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
        float phase = rt->soundboardPhase;
        float inc = (kTwoPi * kSourceToneHz) / (float)SAMPLE_RATE;
        float env = rt->soundboardEnv;
        uint32_t clipFrames = rt->activeClipFrames;
        uint32_t clipRate = rt->activeClipRate;
        uint8_t clipChannels = rt->activeClipChannels;
        const AudioSfxClipSlot &clip = ctx->sfxSlots[rt->activeClipSlot & 1U];

        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            float target = (rt->soundboardBurstRemaining > 0U) ? 1.0f : 0.0f;
            if (target > env) {
                env += 0.02f;
                if (env > target) {
                    env = target;
                }
            } else {
                env *= 0.997f;
                if (env < 0.0001f) {
                    env = 0.0f;
                }
            }

            float s = 0.0f;

            if (rt->soundboardBurstRemaining > 0U &&
                clipFrames > 0U &&
                clipRate > 0U &&
                (clipChannels == 1U || clipChannels == 2U) &&
                rt->activeClipPos < clipFrames) {
                uint32_t posA = rt->activeClipPos;
                uint32_t posB = (posA + 1U < clipFrames) ? (posA + 1U) : posA;
                uint8_t ch0 = 0;
                float a = (float)clip.pcm[posA * clipChannels + ch0] / 32768.0f;
                float b = (float)clip.pcm[posB * clipChannels + ch0] / 32768.0f;
                s = (a + ((b - a) * rt->activeClipFrac)) * env * kSoundboardClipGain;
                if (s > 0.98f) {
                    s = 0.98f;
                }
                if (s < -0.98f) {
                    s = -0.98f;
                }

                float clipStep = (float)clipRate / (float)SAMPLE_RATE;
                rt->activeClipFrac += clipStep;
                while (rt->activeClipFrac >= 1.0f && rt->activeClipPos < clipFrames) {
                    ++rt->activeClipPos;
                    rt->activeClipFrac -= 1.0f;
                }
            }

            for (uint8_t ch = 0; ch < outChannels; ++ch) {
                rt->outputs[nodeIndex][ch][frame] = s;
            }
            phase += inc;
            if (phase >= kTwoPi) {
                phase -= kTwoPi;
            }

            if (rt->soundboardBurstRemaining > 0U) {
                --rt->soundboardBurstRemaining;
            }
        }
        rt->soundboardPhase = phase;
        rt->soundboardEnv = env;
        return;
    }

    // Placeholder for live capture sources. They currently produce silence.
    for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
        for (uint8_t ch = 0; ch < outChannels; ++ch) {
            rt->outputs[nodeIndex][ch][frame] = 0.0f;
        }
    }
}

static void routeCompiledEdge(RuntimeGraph *rt, const RuntimeGraph::CompiledRoute &route) {
    if (!rt) {
        return;
    }

    const float *src = rt->outputs[route.srcNode][route.srcChannel];
    float *dst = rt->inputs[route.dstNode][route.dstChannel];
    for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
        dst[frame] += src[frame];
    }
}

static void processNode(RuntimeGraph *rt, uint16_t nodeIndex) {
    if (!rt || nodeIndex >= rt->snapshot.thingCount) {
        return;
    }

    const AudioGraphThingInfo &thing = rt->snapshot.things[nodeIndex];
    uint8_t inChannels = (thing.inputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.inputs;
    uint8_t outChannels = (thing.outputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.outputs;
    uint8_t copyChannels = (inChannels < outChannels) ? inChannels : outChannels;

    if (copyChannels == 0 || outChannels == 0) {
        return;
    }

    float gain = (rt->nodeKind[nodeIndex] == NODE_EFFECT) ? 0.85f : 1.0f;
    for (uint8_t ch = 0; ch < copyChannels; ++ch) {
        for (uint32_t frame = 0; frame < BUFFER_FRAMES; ++frame) {
            float s = rt->inputs[nodeIndex][ch][frame] * gain;
            if (s > 1.0f) {
                s = 1.0f;
            }
            if (s < -1.0f) {
                s = -1.0f;
            }
            rt->outputs[nodeIndex][ch][frame] = s;
        }
    }
}

static void rebuildRuntimeGraph(RuntimeGraph *rt, const AudioGraphState &graph) {
    if (!rt) {
        return;
    }

    bool topologyChanged = !sameThingLayout(graph, rt->snapshot);

    rt->snapshot = graph;
    rt->sourceNodeCount = 0;
    rt->processNodeCount = 0;
    rt->sinkNodeCount = 0;
    rt->sourceRouteCount = 0;
    rt->processRouteCount = 0;
    rt->soundboardNodeIndex = -1;

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
        rt->nodeKind[i] = classifyNode(rt->snapshot.things[i]);
        if (thingIdEquals(rt->snapshot.things[i], "soundboard_out")) {
            rt->soundboardNodeIndex = (int16_t)i;
        }
        if (rt->nodeKind[i] == NODE_SOURCE && rt->sourceNodeCount < AUDIO_GRAPH_MAX_THINGS) {
            rt->sourceNodes[rt->sourceNodeCount++] = i;
        }
        if ((rt->nodeKind[i] == NODE_EFFECT || rt->nodeKind[i] == NODE_PASS) &&
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
                if (parseCaptureThing(thing.id, &card, &device)) {
                    RuntimeGraph::AlsaCaptureStream &s = rt->capture[rt->captureCount];
                    memset(&s, 0, sizeof(s));
                    s.active = 1;
                    s.fd = -1;
                    s.nodeIndex = i;
                    s.card = card;
                    s.device = device;
                    s.sampleRate = SAMPLE_RATE;
                    s.channels = (thing.outputs == 0) ? 1U : ((thing.outputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.outputs);
                    snprintf(s.path, sizeof(s.path), "/dev/snd/pcmC%uD%uc", (unsigned)card, (unsigned)device);
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
                if (parsePlaybackThing(thing.id, &card, &device)) {
                    RuntimeGraph::AlsaPlaybackStream &s = rt->playback[rt->playbackCount];
                    memset(&s, 0, sizeof(s));
                    s.active = 1;
                    s.fd = -1;
                    s.nodeIndex = i;
                    s.card = card;
                    s.device = device;
                    s.sampleRate = SAMPLE_RATE;
                    s.channels = (thing.inputs == 0) ? 1U : ((thing.inputs > kMaxChannelsPerThing) ? kMaxChannelsPerThing : thing.inputs);
                    snprintf(s.path, sizeof(s.path), "/dev/snd/pcmC%uD%up", (unsigned)card, (unsigned)device);
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
                if (parseCaptureThing(thing.id, &card, &device)) {
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
                if (parsePlaybackThing(thing.id, &card, &device)) {
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
        routeCompiledEdge(rt, rt->sourceRoutes[i]);
    }

    // Process transform/effect nodes from their accumulated input.
    for (uint16_t i = 0; i < rt->processNodeCount; ++i) {
        processNode(rt, rt->processNodes[i]);
    }

    // Route transformed output onward (typically into sinks).
    for (uint16_t i = 0; i < rt->processRouteCount; ++i) {
        routeCompiledEdge(rt, rt->processRoutes[i]);
    }

    rt->blocksProcessed++;
    updateChannelLevels(ctx, rt);

    servicePlaybackIo(rt);
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

    printf("[AUDIO] [INFO] graph block=%llu generation=%u sink_peak=%.3f\n",
           (unsigned long long)rt->blocksProcessed,
           (unsigned)rt->snapshot.generation,
           peak);

    if (rt->captureCount > 0) {
        const RuntimeGraph::AlsaCaptureStream &s = rt->capture[0];
        printf("[AUDIO] [INFO] src ratio=%.5f ring_fill=%u/%u\n",
               s.src.ratio,
               (unsigned)s.ringCount,
               (unsigned)kCaptureRingFrames);
        if (s.fd < 0) {
            printf("[AUDIO] [WARN] capture stream offline: %s\n", s.path);
        }
    }

    if (rt->playbackCount > 0) {
        const RuntimeGraph::AlsaPlaybackStream &p = rt->playback[0];
        if (p.fd < 0) {
            printf("[AUDIO] [WARN] playback stream offline: %s\n", p.path);
        } else {
            printf("[AUDIO] [INFO] playback stream active: %s (%uch @ %u)\n",
                   p.path,
                   (unsigned)p.channels,
                   (unsigned)p.sampleRate);
        }
    }
}

static void *audioProcessingThreadMain(void *arg) {
    AudioContext *ctx = reinterpret_cast<AudioContext *>(arg);
    if (!ctx) {
        return nullptr;
    }

    RuntimeGraph runtime = {};
    uint32_t lastGraphGeneration = 0;
    uint32_t lastGraphSeq = 0;
    timespec nextDeadline = monotonicNow();
    addNs(&nextDeadline, blockPeriodNs());

    configureRealtimeScheduling();

    while (ctx->processingThreadRun) {
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
            rebuildRuntimeGraph(&runtime, graphSnapshot);
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

int AudioContext::setupThreads() {
    if (processingThreadStarted) {
        return RET_OK;
    }

    int initGraphRc = forceRescan();
    if (initGraphRc == RET_ERR) {
        return RET_ERR;
    }

    processingThreadRun = 1;
    if (pthread_create(&processingThread, nullptr, audioProcessingThreadMain, this) != 0) {
        processingThreadRun = 0;
        return RET_ERR;
    }

    processingThreadStarted = 1;
    return (initGraphRc == RET_WARN) ? RET_WARN : RET_OK;
}
