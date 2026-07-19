#include "midi/context.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "audio/context.hpp"
#include "audio/processing_fx.hpp"
#include "config/context.hpp"

namespace {

constexpr uint64_t kProbeIntervalMs = 1200;
constexpr uint64_t kMidiMapReloadIntervalMs = 1000;

enum LightStateKind : uint8_t {
    LIGHT_STATE_MAPPED = 0,
    LIGHT_STATE_PLAYING = 1,
    LIGHT_STATE_STOP_ALL = 2,
    LIGHT_STATE_ACTION = 3,
};

struct MidiCandidate {
    char path[256];
    int cardIndex;
    int isUsb;
};

static uint64_t monotonicMillis() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static int midiNameMatchesNode(const char *name) {
    return (name && strncmp(name, "midiC", 5) == 0) ? 1 : 0;
}

static int midiCardIndexFromName(const char *name) {
    if (!midiNameMatchesNode(name)) {
        return -1;
    }

    const char *start = name + 5;
    const char *end = strchr(start, 'D');
    if (!end || end == start) {
        return -1;
    }

    int value = 0;
    for (const char *p = start; p < end; ++p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        value = (value * 10) + (*p - '0');
    }
    return value;
}

static int midiDeviceIsUsb(const char *nodeName) {
    if (!nodeName || !nodeName[0]) {
        return 0;
    }

    char sysfsPath[PATH_MAX];
    char resolved[PATH_MAX];
    int n = snprintf(sysfsPath, sizeof(sysfsPath), "/sys/class/sound/%s/device", nodeName);
    if (n <= 0 || (size_t)n >= sizeof(sysfsPath)) {
        return 0;
    }

    if (!realpath(sysfsPath, resolved)) {
        return 0;
    }

    return strstr(resolved, "/usb") ? 1 : 0;
}

static int midiCandidateCmp(const void *a, const void *b) {
    const MidiCandidate *ma = (const MidiCandidate *)a;
    const MidiCandidate *mb = (const MidiCandidate *)b;

    int aRank = ma->isUsb ? 0 : 1;
    int bRank = mb->isUsb ? 0 : 1;
    if (aRank != bRank) {
        return aRank - bRank;
    }
    if (ma->cardIndex != mb->cardIndex) {
        return ma->cardIndex - mb->cardIndex;
    }
    return strcmp(ma->path, mb->path);
}

static size_t midiCollectCandidates(MidiCandidate *out, size_t cap) {
    DIR *dir = opendir("/dev/snd");
    if (!dir) {
        return 0;
    }

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < cap) {
        if (!midiNameMatchesNode(entry->d_name)) {
            continue;
        }

        int cardIndex = midiCardIndexFromName(entry->d_name);
        if (cardIndex < 0) {
            continue;
        }

        int n = snprintf(out[count].path, sizeof(out[count].path), "/dev/snd/%s", entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(out[count].path)) {
            continue;
        }

        out[count].cardIndex = cardIndex;
        out[count].isUsb = midiDeviceIsUsb(entry->d_name);
        ++count;
    }

    closedir(dir);

    if (count > 1) {
        qsort(out, count, sizeof(out[0]), midiCandidateCmp);
    }

    return count;
}

static int midiStatusDataLen(uint8_t status) {
    uint8_t kind = (uint8_t)(status & 0xF0);
    if (kind == 0xC0 || kind == 0xD0) {
        return 1;
    }
    return 2;
}

static int midiParseByte(MidiContext *midi,
                         uint8_t byte,
                         uint8_t *status,
                         uint8_t *d0,
                         uint8_t *d1) {
    if (!midi || !status || !d0 || !d1) {
        return 0;
    }

    if (byte >= 0xF8) {
        return 0;
    }

    if (byte & 0x80) {
        if (byte >= 0xF0) {
            midi->runningStatus = 0;
            midi->dataHave = 0;
            midi->dataNeed = 0;
            return 0;
        }

        midi->runningStatus = byte;
        midi->dataHave = 0;
        midi->dataNeed = midiStatusDataLen(byte);
        return 0;
    }

    if (midi->runningStatus == 0 || midi->dataNeed <= 0) {
        return 0;
    }

    if (midi->dataHave < 2) {
        midi->dataBuf[midi->dataHave++] = byte;
    }

    if (midi->dataHave >= midi->dataNeed) {
        *status = midi->runningStatus;
        *d0 = midi->dataBuf[0];
        *d1 = (midi->dataNeed > 1) ? midi->dataBuf[1] : 0;
        midi->dataHave = 0;
        return 1;
    }

    return 0;
}

static int midiPathSafe(const char *s) {
    if (!s || !s[0]) {
        return 0;
    }
    if (strstr(s, "..") != NULL) {
        return 0;
    }
    if (strchr(s, '\\') != NULL) {
        return 0;
    }
    return 1;
}

static int buildAbsoluteSfxPath(const char *configured, char *out, size_t outSz) {
    if (!configured || !out || outSz == 0 || !midiPathSafe(configured)) {
        return RET_ERR;
    }

    if (configured[0] == '/') {
        if (strncmp(configured, SFX_ROOT_DIR "/", strlen(SFX_ROOT_DIR) + 1) != 0 &&
            strcmp(configured, SFX_ROOT_DIR) != 0) {
            return RET_ERR;
        }
        int n = snprintf(out, outSz, "%s", configured);
        return (n > 0 && (size_t)n < outSz) ? RET_OK : RET_ERR;
    }

    int n = snprintf(out, outSz, "%s/%s", SFX_ROOT_DIR, configured);
    return (n > 0 && (size_t)n < outSz) ? RET_OK : RET_ERR;
}

static uint8_t resolveSoundMode(const MidiMapData &data, const char *sfxPath) {
    if (!sfxPath || !sfxPath[0]) {
        return SOUNDBOARD_MODE_PLAY;
    }

    uint32_t count = data.soundModeCount;
    if (count > MIDI_SOUND_MODES_MAX) {
        count = MIDI_SOUND_MODES_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(data.soundModes[i].sfxPath, sfxPath) == 0) {
            return (data.soundModes[i].mode == SOUNDBOARD_MODE_HOLD) ? SOUNDBOARD_MODE_HOLD : SOUNDBOARD_MODE_PLAY;
        }
    }

    return SOUNDBOARD_MODE_PLAY;
}

static uint8_t resolveMappedAction(const MidiMapData &data, uint8_t note) {
    uint32_t count = data.actionMappingCount;
    if (count > MIDI_ACTION_MAPPINGS_MAX) {
        count = MIDI_ACTION_MAPPINGS_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (data.actionMappings[i].note == note) {
            return data.actionMappings[i].action;
        }
    }
    return MIDI_ACTION_NONE;
}

static const char *resolveEffectToggleTarget(const MidiMapData &data, uint8_t note) {
    uint32_t count = data.effectToggleMappingCount;
    if (count > MIDI_EFFECT_TOGGLE_MAPPINGS_MAX) {
        count = MIDI_EFFECT_TOGGLE_MAPPINGS_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (data.effectToggleMappings[i].note == note && data.effectToggleMappings[i].effectId[0]) {
            return data.effectToggleMappings[i].effectId;
        }
    }
    return NULL;
}

static void copyBounded(char *dst, size_t dstSz, const char *src) {
    if (!dst || dstSz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t n = strnlen(src, dstSz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static uint8_t resolveLightVelocity(const MidiMapData &data,
                                    const char *sfxPath,
                                    uint8_t stateKind) {
    const MidiLightGlobal &gl = data.globalLight;
    uint8_t fallback = gl.mappedVel;
    if (stateKind == LIGHT_STATE_PLAYING) {
        fallback = gl.playingVel;
    } else if (stateKind == LIGHT_STATE_STOP_ALL || stateKind == LIGHT_STATE_ACTION) {
        fallback = gl.stopAllVel;
    }

    if (!sfxPath || !sfxPath[0]) {
        return fallback;
    }

    uint32_t count = data.soundLightCount;
    if (count > MIDI_SOUND_LIGHTS_MAX) {
        count = MIDI_SOUND_LIGHTS_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(data.soundLights[i].sfxPath, sfxPath) != 0) {
            continue;
        }
        if (stateKind == LIGHT_STATE_MAPPED && data.soundLights[i].hasMapped) {
            return data.soundLights[i].mappedVel;
        }
        if (stateKind == LIGHT_STATE_PLAYING && data.soundLights[i].hasPlaying) {
            return data.soundLights[i].playingVel;
        }
        return fallback;
    }

    return fallback;
}

static int sfxInPlayingList(const MidiContext *midi, const char *sfxPath) {
    if (!midi || !sfxPath || !sfxPath[0]) {
        return 0;
    }
    uint32_t count = midi->cachedPlayingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        count = MIDI_MAPPINGS_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(midi->cachedPlayingSfx[i], sfxPath) == 0) {
            return 1;
        }
    }
    return 0;
}

} // namespace

MidiContext::MidiContext(Audiox *context)
    : app(context),
      fd(-1),
      connected(0),
      devPath(),
      runningStatus(0),
      dataBuf(),
      dataHave(0),
      dataNeed(0),
      nextProbeMs(0),
      lastNoteSeq(0),
      lastNote(0),
      lastVelocity(0),
      lastCcSeq(0),
      lastCc(0),
      lastCcValue(0),
      cachedMidiMap(),
      nextMidiMapReloadMs(0),
      cachedPlayingSeq(0),
      cachedPlayingCount(0),
      cachedPlayingSfx(),
      ledStateByNote(),
    heldNote(255),
    holdActive(0),
    samplerModeActive(0),
    samplerSelectedValid(0),
    samplerSelectedSfx() {
    if (app) {
        app->midi = this;
        if (app->config) {
            cachedMidiMap = app->config->readMidiMapFile();
        }
    }
    for (int i = 0; i < 128; ++i) {
        ledStateByNote[i] = 255;
    }
}

void MidiContext::disconnect() {
    if (fd >= 0) {
        close(fd);
    }
    fd = -1;

    if (connected) {
        printf("[MIDI] DISCONNECTED\n");
    }
    connected = 0;
    runningStatus = 0;
    dataHave = 0;
    dataNeed = 0;
    devPath[0] = '\0';
    heldNote = 255;
    holdActive = 0;
    for (int i = 0; i < 128; ++i) {
        ledStateByNote[i] = 255;
    }
}

void MidiContext::sendRawMidi(uint8_t b0, uint8_t b1, uint8_t b2) {
    if (fd < 0 || !connected) {
        return;
    }
    uint8_t msg[3] = {b0, b1, b2};
    ssize_t nw = write(fd, msg, sizeof(msg));
    (void)nw;
}

void MidiContext::sendLightNote(uint8_t note, uint8_t channel, uint8_t velocity) {
    if (fd < 0 || !connected) {
        return;
    }
    uint8_t ch = channel & 0x0Fu;
    // Note-on with velocity > 0 lights button; velocity=0 is equivalent to note-off.
    sendRawMidi((uint8_t)(0x90u | ch), note & 0x7Fu, velocity & 0x7Fu);
}

void MidiContext::refreshAllLighting() {
    if (fd < 0 || !connected) {
        return;
    }
    for (int i = 0; i < 128; ++i) {
        ledStateByNote[i] = 255;
    }
    refreshLightingFromState();
}

void MidiContext::refreshLightingFromState() {
    if (fd < 0 || !connected) {
        return;
    }

    const MidiLightGlobal &gl = cachedMidiMap.globalLight;
    uint8_t desired[128];
    memset(desired, 0, sizeof(desired));

    uint32_t count = cachedMidiMap.mappingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        count = MIDI_MAPPINGS_MAX;
    }

    for (uint32_t i = 0; i < count; ++i) {
        const MidiMapData::MidiMapping &m = cachedMidiMap.mappings[i];
        if (!m.sfxPath[0]) {
            continue;
        }
        uint8_t note = m.note & 0x7Fu;

        if (samplerModeActive) {
            desired[note] = resolveLightVelocity(cachedMidiMap, m.sfxPath, LIGHT_STATE_PLAYING);
            continue;
        }

        uint8_t stateKind = LIGHT_STATE_MAPPED;
        uint8_t mode = resolveSoundMode(cachedMidiMap, m.sfxPath);
        if (mode == SOUNDBOARD_MODE_HOLD && holdActive && heldNote == note) {
            stateKind = LIGHT_STATE_PLAYING;
        } else if (sfxInPlayingList(this, m.sfxPath)) {
            stateKind = LIGHT_STATE_PLAYING;
        }

        desired[note] = resolveLightVelocity(cachedMidiMap, m.sfxPath, stateKind);
    }

    uint32_t actionCount = cachedMidiMap.actionMappingCount;
    if (actionCount > MIDI_ACTION_MAPPINGS_MAX) {
        actionCount = MIDI_ACTION_MAPPINGS_MAX;
    }
    for (uint32_t i = 0; i < actionCount; ++i) {
        const MidiActionMapping &a = cachedMidiMap.actionMappings[i];
        if (a.action != MIDI_ACTION_STOP_ALL && a.action != MIDI_ACTION_SAMPLER_TOGGLE) {
            continue;
        }
        uint8_t note = a.note & 0x7Fu;
        if (a.action == MIDI_ACTION_STOP_ALL) {
            desired[note] = resolveLightVelocity(cachedMidiMap, NULL, LIGHT_STATE_STOP_ALL);
        } else {
            desired[note] = resolveLightVelocity(cachedMidiMap, NULL, LIGHT_STATE_ACTION);
        }
    }

    for (int note = 0; note < 128; ++note) {
        uint8_t target = desired[note];
        if (ledStateByNote[note] == target) {
            continue;
        }
        sendLightNote((uint8_t)note, gl.channel, target);
        ledStateByNote[note] = target;
    }
}

void MidiContext::poll() {
    if (!app || !app->config) {
        return;
    }

    uint64_t now = monotonicMillis();
    if (now >= nextMidiMapReloadMs) {
        cachedMidiMap = app->config->readMidiMapFile();
        nextMidiMapReloadMs = now + kMidiMapReloadIntervalMs;
        refreshLightingFromState();
    }

    if (app->audio) {
        uint32_t playingSeq = app->audio->sfxPlayingSeq.load(std::memory_order_acquire);
        if (playingSeq != cachedPlayingSeq) {
            cachedPlayingSeq = playingSeq;
            {
                std::lock_guard<std::mutex> lock(app->audio->sfxPlayingMutex);
                cachedPlayingCount = app->audio->sfxPlayingCount;
                if (cachedPlayingCount > MIDI_MAPPINGS_MAX) {
                    cachedPlayingCount = MIDI_MAPPINGS_MAX;
                }
                memset(cachedPlayingSfx, 0, sizeof(cachedPlayingSfx));
                for (uint32_t i = 0; i < cachedPlayingCount; ++i) {
                    size_t n = strnlen(app->audio->sfxPlayingBasenames[i], MIDI_SFX_PATH_MAX - 1);
                    memcpy(cachedPlayingSfx[i], app->audio->sfxPlayingBasenames[i], n);
                    cachedPlayingSfx[i][n] = '\0';
                }
            }
            refreshLightingFromState();
        }
    }

    if (fd < 0) {
        if (now < nextProbeMs) {
            return;
        }

        nextProbeMs = now + kProbeIntervalMs;
        MidiCandidate candidates[32];
        size_t count = midiCollectCandidates(candidates, 32);

        int sawUsb = 0;
        for (size_t i = 0; i < count; ++i) {
            if (candidates[i].isUsb) {
                sawUsb = 1;
                break;
            }
        }

        for (size_t i = 0; i < count; ++i) {
            if (sawUsb && !candidates[i].isUsb) {
                continue;
            }

            int maybeFd = open(candidates[i].path, O_RDWR | O_NONBLOCK);
            if (maybeFd < 0) {
                // Fall back to read-only if the device doesn't support write.
                maybeFd = open(candidates[i].path, O_RDONLY | O_NONBLOCK);
                if (maybeFd < 0) {
                    continue;
                }
            }

            fd = maybeFd;
            connected = 1;
            runningStatus = 0;
            dataHave = 0;
            dataNeed = 0;
            copyBounded(devPath, sizeof(devPath), candidates[i].path);
            printf("[MIDI] CONNECTED %s%s (mappings=%u, mapped_vel=%u play_vel=%u)\n",
                   candidates[i].path,
                   candidates[i].isUsb ? " (USB)" : "",
                   (unsigned)cachedMidiMap.mappingCount,
                   (unsigned)cachedMidiMap.globalLight.mappedVel,
                   (unsigned)cachedMidiMap.globalLight.playingVel);
            refreshAllLighting();
            return;
        }

        return;
    }

    if (access(devPath, F_OK) != 0) {
        disconnect();
        nextProbeMs = now + kProbeIntervalMs;
        return;
    }

    struct pollfd pfd;
    memset(&pfd, 0, sizeof(pfd));
    pfd.fd = fd;
    pfd.events = POLLIN | POLLERR | POLLHUP;
    int prc = ::poll(&pfd, 1, 0);
    if (prc > 0 && (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
        disconnect();
        nextProbeMs = now + kProbeIntervalMs;
        return;
    }

    uint8_t buf[128];
    while (1) {
        ssize_t n = read(fd, buf, sizeof(buf));
        if (n > 0) {
            for (ssize_t i = 0; i < n; ++i) {
                uint8_t status = 0;
                uint8_t d0 = 0;
                uint8_t d1 = 0;
                if (!midiParseByte(this, buf[i], &status, &d0, &d1)) {
                    continue;
                }

                const uint8_t kind = (uint8_t)(status & 0xF0);
                const uint8_t channel = (uint8_t)(status & 0x0F);
                int isNoteOn = (kind == 0x90 && d1 > 0);
                int isNoteOff = (kind == 0x80) || (kind == 0x90 && d1 == 0);
                int isCc = (kind == 0xB0);

                if (isCc) {
                    ++lastCcSeq;
                    lastCc = d0;
                    lastCcValue = d1;

                    float gain = ((float)d1 / 127.0f) * 2.0f;
                    uint32_t cvCount = cachedMidiMap.ccVolumeMappingCount;
                    if (cvCount > MIDI_CC_VOLUME_MAPPINGS_MAX) {
                        cvCount = MIDI_CC_VOLUME_MAPPINGS_MAX;
                    }
                    for (uint32_t cvi = 0; cvi < cvCount; ++cvi) {
                        if (cachedMidiMap.ccVolumeMappings[cvi].cc == d0 &&
                            cachedMidiMap.ccVolumeMappings[cvi].thingId[0] &&
                            app->audio) {
                            app->audio->setThingGain(cachedMidiMap.ccVolumeMappings[cvi].thingId, gain);
                            printf("[MIDI] CC ch=%u n=%u v=%u -> volume %s=%.3f\n",
                                   (unsigned)channel,
                                   (unsigned)d0,
                                   (unsigned)d1,
                                   cachedMidiMap.ccVolumeMappings[cvi].thingId,
                                   (double)gain);
                        }
                    }

                    uint32_t fxCcCount = cachedMidiMap.effectCcMappingCount;
                    if (fxCcCount > MIDI_EFFECT_CC_MAPPINGS_MAX) {
                        fxCcCount = MIDI_EFFECT_CC_MAPPINGS_MAX;
                    }
                    for (uint32_t fxi = 0; fxi < fxCcCount; ++fxi) {
                        const MidiEffectCcMapping &fxMap = cachedMidiMap.effectCcMappings[fxi];
                        if (fxMap.cc != d0 || !fxMap.effectId[0] || !fxMap.param[0] || !app->audio) {
                            continue;
                        }

                        float normalized = (float)d1 / 127.0f;
                        if (app->audio->setEffectParamNormalized(fxMap.effectId,
                                                                  fxMap.param,
                                                                  normalized) == RET_OK) {
                            printf("[MIDI] CC ch=%u n=%u v=%u -> effect %s.%s=%.3f\n",
                                   (unsigned)channel,
                                   (unsigned)d0,
                                   (unsigned)d1,
                                   fxMap.effectId,
                                   fxMap.param,
                                   (double)normalized);
                        }
                    }
                    continue;
                }

                if (!isNoteOn && !isNoteOff) {
                    continue;
                }

                if (isNoteOff) {
                    // Some controllers send true NOTE OFF (0x80), others NOTE ON vel=0.
                    // Treat both as release and do not depend on mapping lookup.
                    // Stop on any NOTE OFF while hold is active, because some controllers
                    // emit inconsistent note numbers/channels on release.
                    if (holdActive && app->audio) {
                        app->audio->stopHeldSfx();
                        holdActive = 0;
                        heldNote = 255;
                        refreshLightingFromState();
                        printf("[MIDI] NOTE OFF status=0x%02X n=%u v=%u -> hold release\n",
                               (unsigned)status,
                               (unsigned)d0,
                               (unsigned)d1);
                    } else {
                        printf("[MIDI] NOTE OFF status=0x%02X n=%u v=%u\n",
                               (unsigned)status,
                               (unsigned)d0,
                               (unsigned)d1);
                    }
                    continue;
                }

                if (isNoteOn) {
                    ++lastNoteSeq;
                    lastNote = d0;
                    lastVelocity = d1;
                }

                int samplerKeyboardChannel = -1;
                uint8_t samplerRootNote = cachedMidiMap.samplerRootNote;
                if (cachedMidiMap.samplerKeyboardEnabled) {
                    samplerKeyboardChannel = (int)(cachedMidiMap.samplerKeyboardChannel & 0x0Fu);
                }

                if (isNoteOn && samplerSelectedValid &&
                    samplerKeyboardChannel >= 0 &&
                    channel == (uint8_t)samplerKeyboardChannel &&
                    app->audio) {
                    char samplerSfxPath[256];
                    if (buildAbsoluteSfxPath(samplerSelectedSfx, samplerSfxPath, sizeof(samplerSfxPath)) == RET_OK) {
                        int semitones = (int)d0 - (int)samplerRootNote;
                        float pitchRatio = audiox::processing::semitoneToPitchRatio(semitones);
                        int samplerRc = app->audio->triggerSfxPitch(samplerSfxPath, pitchRatio);
                        if (samplerRc == RET_OK) {
                            printf("[MIDI] NOTE ON ch=%u n=%u v=%u -> sampler %s ratio=%.3f\n",
                                   (unsigned)channel,
                                   (unsigned)d0,
                                   (unsigned)d1,
                                   samplerSfxPath,
                                   (double)pitchRatio);
                        } else {
                            printf("[MIDI] [WARN] sampler trigger failed ch=%u n=%u sfx=%s rc=%d\n",
                                   (unsigned)channel,
                                   (unsigned)d0,
                                   samplerSfxPath,
                                   samplerRc);
                        }
                    }
                    continue;
                }

                uint8_t mappedAction = resolveMappedAction(cachedMidiMap, d0);
                if (mappedAction == MIDI_ACTION_STOP_ALL) {
                    if (app->audio) {
                        app->audio->stopAllSfx();
                    }
                    holdActive = 0;
                    heldNote = 255;
                    refreshLightingFromState();
                    printf("[MIDI] NOTE ON n=%u v=%u -> action stop_all\n", (unsigned)d0, (unsigned)d1);
                    continue;
                }

                const char *effectToggleId = resolveEffectToggleTarget(cachedMidiMap, d0);
                if (effectToggleId && app->audio) {
                    int toggleRc = app->audio->toggleEffectEnabled(effectToggleId);
                    if (toggleRc == RET_OK) {
                        printf("[MIDI] NOTE ON n=%u v=%u -> toggled bypass %s\n",
                               (unsigned)d0,
                               (unsigned)d1,
                               effectToggleId);
                    } else {
                        printf("[MIDI] [WARN] NOTE ON n=%u -> effect toggle failed for %s\n",
                               (unsigned)d0,
                               effectToggleId);
                    }
                    continue;
                }
                if (mappedAction == MIDI_ACTION_SAMPLER_TOGGLE) {
                    samplerModeActive = samplerModeActive ? 0U : 1U;
                    refreshLightingFromState();
                    printf("[MIDI] NOTE ON n=%u v=%u -> action sampler_toggle (%s)\n",
                           (unsigned)d0,
                           (unsigned)d1,
                           samplerModeActive ? "on" : "off");
                    continue;
                }

                // Look up note in midi_map.txt.
                const MidiMapData::MidiMapping *mapping = NULL;
                uint32_t mmCount = cachedMidiMap.mappingCount;
                if (mmCount > MIDI_MAPPINGS_MAX) {
                    mmCount = MIDI_MAPPINGS_MAX;
                }
                for (uint32_t mi = 0; mi < mmCount; ++mi) {
                    if (cachedMidiMap.mappings[mi].note == d0 &&
                        cachedMidiMap.mappings[mi].sfxPath[0]) {
                        mapping = &cachedMidiMap.mappings[mi];
                        break;
                    }
                }
                if (!mapping) {
                    continue;
                }

                if (samplerModeActive) {
                    copyBounded(samplerSelectedSfx, sizeof(samplerSelectedSfx), mapping->sfxPath);
                    samplerSelectedValid = mapping->sfxPath[0] ? 1U : 0U;
                    refreshLightingFromState();
                    printf("[MIDI] NOTE ON n=%u v=%u -> sampler selected %s\n",
                           (unsigned)d0,
                           (unsigned)d1,
                           samplerSelectedSfx);
                    continue;
                }

                char sfxPath[256];
                if (buildAbsoluteSfxPath(mapping->sfxPath, sfxPath, sizeof(sfxPath)) != RET_OK) {
                    printf("[MIDI] [WARN] invalid mapped SFX path for note %u: %s\n",
                           (unsigned)d0,
                           mapping->sfxPath);
                    continue;
                }

                if (!app->audio) {
                    printf("[MIDI] [WARN] note %u mapped to %s, but audio subsystem is unavailable\n",
                           (unsigned)d0,
                           sfxPath);
                    continue;
                }

                uint8_t mode = resolveSoundMode(cachedMidiMap, mapping->sfxPath);
                int holdMode = (mode == SOUNDBOARD_MODE_HOLD) ? 1 : 0;

                int triggerRc = RET_ERR;
                if (holdMode) {
                    if (holdActive && heldNote != d0) {
                        app->audio->stopHeldSfx();
                    }
                    triggerRc = app->audio->startHeldSfx(sfxPath);
                    if (triggerRc == RET_OK) {
                        holdActive = 1;
                        heldNote = d0;
                        refreshLightingFromState();
                        printf("[MIDI] NOTE ON n=%u v=%u -> hold %s\n", (unsigned)d0, (unsigned)d1, sfxPath);
                    }
                } else {
                    triggerRc = app->audio->triggerSfx(sfxPath);
                    if (triggerRc == RET_OK) {
                        printf("[MIDI] NOTE ON n=%u v=%u -> %s\n", (unsigned)d0, (unsigned)d1, sfxPath);
                    }
                }

                if (triggerRc != RET_OK) {
                    printf("[MIDI] [WARN] note %u mapped to %s but trigger failed (%d)\n",
                           (unsigned)d0,
                           sfxPath,
                           triggerRc);
                }
            }
            continue;
        }

        if (n == 0) {
            disconnect();
            nextProbeMs = now + kProbeIntervalMs;
            return;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }

        disconnect();
        nextProbeMs = now + kProbeIntervalMs;
        return;
    }
}