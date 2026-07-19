#include "audio/context.hpp"

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <stdint.h>

#include "config/context.hpp"

namespace {

static uint64_t audioMonotonicMs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static uint32_t le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static int loadPcm16WavIntoSlot(const char *path, AudioSfxClipSlot *slot) {
    if (!path || !slot) {
        return RET_ERR;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return RET_WARN;
    }

    uint8_t header[12];
    if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        return RET_ERR;
    }

    if (memcmp(header, "RIFF", 4) != 0 || memcmp(header + 8, "WAVE", 4) != 0) {
        fclose(fp);
        return RET_ERR;
    }

    uint16_t fmtTag = 0;
    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;
    uint32_t dataSize = 0;
    long dataOffset = -1;

    while (1) {
        uint8_t chunkHdr[8];
        if (fread(chunkHdr, 1, sizeof(chunkHdr), fp) != sizeof(chunkHdr)) {
            break;
        }
        uint32_t chunkSize = le32(chunkHdr + 4);

        if (memcmp(chunkHdr, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            uint32_t readSize = (chunkSize > sizeof(fmt)) ? sizeof(fmt) : chunkSize;
            if (fread(fmt, 1, readSize, fp) != readSize) {
                fclose(fp);
                return RET_ERR;
            }
            if (chunkSize > readSize) {
                if (fseek(fp, (long)(chunkSize - readSize), SEEK_CUR) != 0) {
                    fclose(fp);
                    return RET_ERR;
                }
            }

            if (readSize >= 16) {
                fmtTag = le16(fmt + 0);
                channels = le16(fmt + 2);
                sampleRate = le32(fmt + 4);
                bitsPerSample = le16(fmt + 14);
            }
        } else if (memcmp(chunkHdr, "data", 4) == 0) {
            dataSize = chunkSize;
            dataOffset = ftell(fp);
            if (fseek(fp, (long)chunkSize, SEEK_CUR) != 0) {
                fclose(fp);
                return RET_ERR;
            }
        } else {
            if (fseek(fp, (long)chunkSize, SEEK_CUR) != 0) {
                fclose(fp);
                return RET_ERR;
            }
        }

        if ((chunkSize & 1U) != 0U) {
            if (fseek(fp, 1, SEEK_CUR) != 0) {
                fclose(fp);
                return RET_ERR;
            }
        }
    }

    if (fmtTag != 1 || (channels != 1 && channels != 2) || bitsPerSample != 16 || sampleRate == 0 || dataOffset < 0 || dataSize == 0) {
        fclose(fp);
        return RET_ERR;
    }

    uint32_t bytesPerFrame = (uint32_t)channels * 2U;
    uint32_t frames = dataSize / bytesPerFrame;
    if (frames > AUDIO_SFX_MAX_FRAMES) {
        frames = AUDIO_SFX_MAX_FRAMES;
    }

    if (fseek(fp, dataOffset, SEEK_SET) != 0) {
        fclose(fp);
        return RET_ERR;
    }

    size_t needSamples = (size_t)frames * (size_t)channels;
    if (fread(slot->pcm, sizeof(int16_t), needSamples, fp) != needSamples) {
        fclose(fp);
        return RET_ERR;
    }

    fclose(fp);

    slot->frames = frames;
    slot->channels = (uint8_t)channels;
    slot->sampleRate = sampleRate;
    return RET_OK;
}

static int queueSfxClip(AudioContext *ctx, const char *sfxPath, int holdStart) {
    if (!ctx || !sfxPath || !sfxPath[0]) {
        return RET_ERR;
    }

    if (strncmp(sfxPath, SFX_ROOT_DIR "/", strlen(SFX_ROOT_DIR) + 1) != 0) {
        printf("[AUDIO] [WARN] refusing SFX outside %s: %s\n", SFX_ROOT_DIR, sfxPath);
        return RET_ERR;
    }

    uint32_t active = ctx->sfxActiveSlot.load(std::memory_order_relaxed) & 1U;
    uint32_t inactive = active ^ 1U;
    int loadRc = loadPcm16WavIntoSlot(sfxPath, &ctx->sfxSlots[inactive]);
    if (loadRc == RET_OK) {
        ctx->sfxActiveSlot.store(inactive, std::memory_order_release);
    } else if (loadRc == RET_WARN) {
        printf("[AUDIO] [WARN] mapped SFX missing: %s\n", sfxPath);
        return RET_WARN;
    } else {
        printf("[AUDIO] [WARN] failed to load WAV clip: %s\n", sfxPath);
        return RET_ERR;
    }

    const char *slash = strrchr(sfxPath, '/');
    const char *basename = slash ? slash + 1 : sfxPath;
    {
        std::lock_guard<std::mutex> lock(ctx->sfxNameMutex);
        size_t n = strnlen(basename, sizeof(ctx->sfxLastTriggeredBasename) - 1);
        memcpy(ctx->sfxLastTriggeredBasename, basename, n);
        ctx->sfxLastTriggeredBasename[n] = '\0';
    }
    ctx->sfxTriggerSeq.fetch_add(1U, std::memory_order_release);

    if (holdStart) {
        uint32_t queued = ctx->pendingSfxHoldStarts.fetch_add(1U, std::memory_order_relaxed) + 1U;
        if ((queued % 64U) == 1U) {
            printf("[AUDIO] [INFO] queued hold-start(s): %u\n", (unsigned)queued);
        }
    } else {
        uint32_t queued = ctx->pendingSfxTriggers.fetch_add(1U, std::memory_order_relaxed) + 1U;
        if ((queued % 64U) == 1U) {
            printf("[AUDIO] [INFO] queued soundboard trigger(s): %u\n", (unsigned)queued);
        }
    }

    return RET_OK;
}

}

AudioContext::AudioContext(Audiox *context) : app(context) {
    if (app) {
        app->audio = this;
    }

    processingThread = (pthread_t)0;
    processingThreadStarted = 0;
    processingThreadRun = 0;

    nextHotplugScanMs = 0;
    nextHandle = 1;
    deviceGeneration = 0;
    pendingSfxTriggers.store(0, std::memory_order_relaxed);
    pendingSfxHoldStarts.store(0, std::memory_order_relaxed);
    pendingSfxHoldStops.store(0, std::memory_order_relaxed);
    pendingSfxStopAll.store(0, std::memory_order_relaxed);
    memset(&sfxSlots, 0, sizeof(sfxSlots));
    sfxActiveSlot.store(0, std::memory_order_relaxed);
    soundboardMode.store(SOUNDBOARD_MODE_PLAY, std::memory_order_relaxed);
    sfxIsPlaying.store(0, std::memory_order_relaxed);
    sfxTriggerSeq.store(0, std::memory_order_relaxed);
    sfxLastTriggeredBasename[0] = '\0';
    memset(&routingGraph, 0, sizeof(routingGraph));
    memset(&routingGraphPublished, 0, sizeof(routingGraphPublished));
    routingGraphSeq.store(0, std::memory_order_relaxed);
    memset(&nodeChannelLevels, 0, sizeof(nodeChannelLevels));

    if (app && app->config) {
        ConfigData cfg = app->config->readConfigFile();
        soundboardMode.store(cfg.soundboardMode, std::memory_order_relaxed);
    }
}

AudioContext::~AudioContext() {
    processingThreadRun = 0;
}

int AudioContext::triggerSfx(const char *sfxPath) {
    return queueSfxClip(this, sfxPath, 0);
}

int AudioContext::startHeldSfx(const char *sfxPath) {
    return queueSfxClip(this, sfxPath, 1);
}

void AudioContext::stopHeldSfx() {
    pendingSfxHoldStops.fetch_add(1U, std::memory_order_relaxed);
}

void AudioContext::stopAllSfx() {
    pendingSfxStopAll.fetch_add(1U, std::memory_order_relaxed);
}

void AudioContext::setSoundboardMode(uint8_t mode) {
    uint8_t normalized = (mode == SOUNDBOARD_MODE_HOLD) ? SOUNDBOARD_MODE_HOLD : SOUNDBOARD_MODE_PLAY;
    soundboardMode.store(normalized, std::memory_order_release);
}

uint8_t AudioContext::getSoundboardMode() const {
    return soundboardMode.load(std::memory_order_acquire);
}

void AudioContext::poll() {
    uint64_t now = audioMonotonicMs();
    if (now < nextHotplugScanMs) {
        return;
    }

    nextHotplugScanMs = now + 1000ULL;
    (void)rescanDevices();
}

int AudioContext::forceRescan() {
    nextHotplugScanMs = 0;
    int rc = rescanDevices();
    int graphRc = reloadRoutingGraph();
    if (rc == RET_ERR || graphRc == RET_ERR) {
        return RET_ERR;
    }
    if (rc == RET_WARN || graphRc == RET_WARN) {
        return RET_WARN;
    }
    return RET_OK;
}

float AudioContext::getChannelLevel(AudioHandle handle, int channelIndex, bool isCapture) const {
    std::lock_guard<std::mutex> lock(devicesMutex);
    
    auto it = devices.find(handle);
    if (it == devices.end()) {
        return 0.0f;
    }
    
    const AudioDeviceInfo &info = it->second;
    
    char thingId[64];
    if (isCapture) {
        snprintf(thingId, sizeof(thingId), "alsa_card%u_dev%u_in",
                 (unsigned)info.cardIndex, (unsigned)info.deviceIndex);
    } else {
        snprintf(thingId, sizeof(thingId), "alsa_card%u_dev%u_out",
                 (unsigned)info.cardIndex, (unsigned)info.deviceIndex);
    }
    
    std::lock_guard<std::mutex> graphLock(routingGraphMutex);
    
    for (uint16_t i = 0; i < routingGraphPublished.thingCount; ++i) {
        if (strcmp(routingGraphPublished.things[i].id, thingId) == 0) {
            if (channelIndex >= 0 && channelIndex < 16) {
                return nodeChannelLevels[i][channelIndex].load(std::memory_order_relaxed);
            }
            break;
        }
    }
    
    return 0.0f;
}
