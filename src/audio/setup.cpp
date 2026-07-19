#include "audio/context.hpp"

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
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

static void clearSfxSlot(AudioSfxClipSlot *slot) {
    if (!slot) {
        return;
    }
    if (slot->pcm) {
        free(slot->pcm);
        slot->pcm = nullptr;
    }
    slot->loaded = 0;
    slot->name[0] = '\0';
    slot->frames = 0;
    slot->sampleRate = 0;
    slot->channels = 0;
    slot->sampleCount = 0;
}

static int loadPcm16WavFile(const char *path, const char *name, AudioSfxClipSlot *slot) {
    if (!path || !name || !slot || !name[0]) {
        return RET_ERR;
    }

    clearSfxSlot(slot);

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

    uint32_t sampleCount = frames * (uint32_t)channels;
    int16_t *pcm = (int16_t *)malloc((size_t)sampleCount * sizeof(int16_t));
    if (!pcm) {
        fclose(fp);
        return RET_ERR;
    }

    if (fread(pcm, sizeof(int16_t), sampleCount, fp) != sampleCount) {
        free(pcm);
        fclose(fp);
        return RET_ERR;
    }

    fclose(fp);

    slot->loaded = 1U;
    size_t n = strnlen(name, sizeof(slot->name) - 1);
    memcpy(slot->name, name, n);
    slot->name[n] = '\0';
    slot->frames = frames;
    slot->channels = (uint8_t)channels;
    slot->sampleRate = sampleRate;
    slot->sampleCount = sampleCount;
    slot->pcm = pcm;
    return RET_OK;
}

static int buildSfxBasename(const char *sfxPath, char *out, size_t outSz) {
    if (!sfxPath || !out || outSz == 0) {
        return RET_ERR;
    }
    if (strncmp(sfxPath, SFX_ROOT_DIR "/", strlen(SFX_ROOT_DIR) + 1) != 0) {
        return RET_ERR;
    }

    const char *slash = strrchr(sfxPath, '/');
    const char *basename = slash ? slash + 1 : sfxPath;
    if (!basename[0]) {
        return RET_ERR;
    }

    size_t n = strnlen(basename, outSz - 1);
    memcpy(out, basename, n);
    out[n] = '\0';
    return RET_OK;
}

static int findLoadedSfxSlot(const AudioContext *ctx, const char *basename) {
    if (!ctx || !basename || !basename[0]) {
        return -1;
    }

    for (uint32_t i = 0; i < AUDIO_SFX_SLOT_COUNT; ++i) {
        const AudioSfxClipSlot &slot = ctx->sfxSlots[i];
        if (!slot.loaded || !slot.name[0]) {
            continue;
        }
        if (strcmp(slot.name, basename) == 0) {
            return (int)i;
        }
    }

    return -1;
}

static int enqueueSfxEvent(AudioContext *ctx, uint8_t slotIndex, int holdStart, float pitchRatio) {
    if (!ctx || slotIndex >= AUDIO_SFX_SLOT_COUNT) {
        return RET_ERR;
    }

    std::lock_guard<std::mutex> lock(ctx->sfxQueueMutex);
    uint32_t write = ctx->sfxQueueWrite.load(std::memory_order_relaxed);
    uint32_t read = ctx->sfxQueueRead.load(std::memory_order_acquire);
    if ((write - read) >= AUDIO_SFX_QUEUE_CAP) {
        ctx->sfxQueueDropped.fetch_add(1U, std::memory_order_relaxed);
        return RET_WARN;
    }

    uint32_t idx = write % AUDIO_SFX_QUEUE_CAP;
    ctx->sfxQueue[idx].slotIndex = slotIndex;
    ctx->sfxQueue[idx].holdStart = holdStart ? 1U : 0U;
    ctx->sfxQueue[idx].pitchRatio = pitchRatio;
    ctx->sfxQueueWrite.store(write + 1U, std::memory_order_release);
    return RET_OK;
}

static int queueSfxByPath(AudioContext *ctx, const char *sfxPath, int holdStart, float pitchRatio) {
    if (!ctx || !sfxPath || !sfxPath[0]) {
        return RET_ERR;
    }

    char basename[MIDI_SFX_PATH_MAX];
    if (buildSfxBasename(sfxPath, basename, sizeof(basename)) != RET_OK) {
        printf("[AUDIO] [WARN] refusing SFX outside %s: %s\n", SFX_ROOT_DIR, sfxPath);
        return RET_ERR;
    }

    int slotIndex = -1;
    {
        std::lock_guard<std::mutex> lock(ctx->sfxBankMutex);
        slotIndex = findLoadedSfxSlot(ctx, basename);
    }

    if (slotIndex < 0) {
        int reloadRc = ctx->reloadSfxBank();
        if (reloadRc == RET_ERR) {
            return RET_ERR;
        }
        std::lock_guard<std::mutex> lock(ctx->sfxBankMutex);
        slotIndex = findLoadedSfxSlot(ctx, basename);
        if (slotIndex < 0) {
            printf("[AUDIO] [WARN] SFX not preloaded: %s\n", basename);
            return RET_WARN;
        }
        ctx->sfxSlotRefs[(uint32_t)slotIndex].fetch_add(1U, std::memory_order_acq_rel);
    } else {
        std::lock_guard<std::mutex> lock(ctx->sfxBankMutex);
        ctx->sfxSlotRefs[(uint32_t)slotIndex].fetch_add(1U, std::memory_order_acq_rel);
    }

    {
        std::lock_guard<std::mutex> lock(ctx->sfxNameMutex);
        size_t n = strnlen(basename, sizeof(ctx->sfxLastTriggeredBasename) - 1);
        memcpy(ctx->sfxLastTriggeredBasename, basename, n);
        ctx->sfxLastTriggeredBasename[n] = '\0';
    }
    ctx->sfxTriggerSeq.fetch_add(1U, std::memory_order_release);

    int queueRc = enqueueSfxEvent(ctx, (uint8_t)slotIndex, holdStart, pitchRatio);
    if (queueRc != RET_OK) {
        ctx->sfxSlotRefs[(uint32_t)slotIndex].fetch_sub(1U, std::memory_order_acq_rel);
        if (queueRc == RET_WARN) {
            printf("[AUDIO] [WARN] soundboard trigger queue full, dropping %s\n", sfxPath);
            return RET_WARN;
        }
        return RET_ERR;
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
    pendingSfxHoldStops.store(0, std::memory_order_relaxed);
    pendingSfxStopAll.store(0, std::memory_order_relaxed);
    memset(&sfxSlots, 0, sizeof(sfxSlots));
    for (uint32_t i = 0; i < AUDIO_SFX_SLOT_COUNT; ++i) {
        sfxSlotRefs[i].store(0U, std::memory_order_relaxed);
    }
    memset(&sfxQueue, 0, sizeof(sfxQueue));
    sfxQueueWrite.store(0U, std::memory_order_relaxed);
    sfxQueueRead.store(0U, std::memory_order_relaxed);
    sfxQueueDropped.store(0U, std::memory_order_relaxed);
    soundboardMode.store(SOUNDBOARD_MODE_PLAY, std::memory_order_relaxed);
    sfxIsPlaying.store(0, std::memory_order_relaxed);
    sfxTriggerSeq.store(0, std::memory_order_relaxed);
    sfxLastTriggeredBasename[0] = '\0';
    sfxPlayingSeq.store(0, std::memory_order_relaxed);
    sfxPlayingCount = 0;
    memset(&sfxPlayingBasenames, 0, sizeof(sfxPlayingBasenames));
    memset(&routingGraph, 0, sizeof(routingGraph));
    memset(&routingGraphPublished, 0, sizeof(routingGraphPublished));
    routingGraphSeq.store(0, std::memory_order_relaxed);
    memset(&nodeChannelLevels, 0, sizeof(nodeChannelLevels));

    if (app && app->config) {
        ConfigData cfg = app->config->readConfigFile();
        soundboardMode.store(cfg.soundboardMode, std::memory_order_relaxed);
    }

    int preloadRc = reloadSfxBank();
    if (preloadRc == RET_ERR) {
        printf("[AUDIO] [WARN] failed to preload soundboard bank at startup\n");
    }
}

AudioContext::~AudioContext() {
    processingThreadRun = 0;
    std::lock_guard<std::mutex> lock(sfxBankMutex);
    for (uint32_t i = 0; i < AUDIO_SFX_SLOT_COUNT; ++i) {
        clearSfxSlot(&sfxSlots[i]);
        sfxSlotRefs[i].store(0U, std::memory_order_relaxed);
    }
}

int AudioContext::triggerSfx(const char *sfxPath) {
    return queueSfxByPath(this, sfxPath, 0, 1.0f);
}

int AudioContext::triggerSfxPitch(const char *sfxPath, float pitchRatio) {
    if (!(pitchRatio > 0.0f)) {
        pitchRatio = 1.0f;
    }
    return queueSfxByPath(this, sfxPath, 0, pitchRatio);
}

int AudioContext::startHeldSfx(const char *sfxPath) {
    return queueSfxByPath(this, sfxPath, 1, 1.0f);
}

int AudioContext::reloadSfxBank() {
    DIR *dir = opendir(SFX_ROOT_DIR);
    if (!dir) {
        return RET_WARN;
    }

    uint8_t seen[AUDIO_SFX_SLOT_COUNT];
    memset(seen, 0, sizeof(seen));
    int rcOverall = RET_OK;

    std::lock_guard<std::mutex> lock(sfxBankMutex);

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (!name || !name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
            continue;
        }

        size_t len = strlen(name);
        if (len < 5 || strcmp(name + (len - 4), ".wav") != 0) {
            continue;
        }

        int existing = findLoadedSfxSlot(this, name);
        uint32_t targetSlot = AUDIO_SFX_SLOT_COUNT;
        if (existing >= 0) {
            targetSlot = (uint32_t)existing;
            if (sfxSlotRefs[targetSlot].load(std::memory_order_acquire) > 0U) {
                seen[targetSlot] = 1U;
                continue;
            }
            clearSfxSlot(&sfxSlots[targetSlot]);
        } else {
            for (uint32_t i = 0; i < AUDIO_SFX_SLOT_COUNT; ++i) {
                if (!sfxSlots[i].loaded && sfxSlotRefs[i].load(std::memory_order_acquire) == 0U) {
                    targetSlot = i;
                    break;
                }
            }
        }

        if (targetSlot >= AUDIO_SFX_SLOT_COUNT) {
            rcOverall = RET_WARN;
            continue;
        }

        char path[256];
        int pn = snprintf(path, sizeof(path), "%s/%s", SFX_ROOT_DIR, name);
        if (pn <= 0 || (size_t)pn >= sizeof(path)) {
            rcOverall = RET_WARN;
            continue;
        }

        AudioSfxClipSlot loaded = {};
        int loadRc = loadPcm16WavFile(path, name, &loaded);
        if (loadRc != RET_OK) {
            clearSfxSlot(&loaded);
            if (loadRc == RET_WARN) {
                rcOverall = RET_WARN;
            } else {
                rcOverall = RET_ERR;
            }
            continue;
        }

        sfxSlots[targetSlot] = loaded;
        seen[targetSlot] = 1U;
    }

    closedir(dir);

    for (uint32_t i = 0; i < AUDIO_SFX_SLOT_COUNT; ++i) {
        if (!sfxSlots[i].loaded) {
            continue;
        }
        if (seen[i]) {
            continue;
        }
        if (sfxSlotRefs[i].load(std::memory_order_acquire) > 0U) {
            continue;
        }
        clearSfxSlot(&sfxSlots[i]);
    }

    return rcOverall;
}

uint32_t AudioContext::loadedSfxCount() const {
    std::lock_guard<std::mutex> lock(sfxBankMutex);
    uint32_t count = 0;
    for (uint32_t i = 0; i < AUDIO_SFX_SLOT_COUNT; ++i) {
        if (sfxSlots[i].loaded) {
            ++count;
        }
    }
    return count;
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
