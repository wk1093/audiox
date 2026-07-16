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
#include "config/context.hpp"

namespace {

constexpr uint64_t kProbeIntervalMs = 1200;
constexpr uint64_t kMidiMapReloadIntervalMs = 1000;

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
      cachedMidiMap(),
      nextMidiMapReloadMs(0),
      currentLitNote(255),
      sfxWasPlaying(0),
      cachedTriggerSeq(0) {
    if (app) {
        app->midi = this;
        if (app->config) {
            cachedMidiMap = app->config->readMidiMapFile();
        }
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
    currentLitNote = 255;
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
    const MidiLightGlobal &gl = cachedMidiMap.globalLight;
    uint32_t count = cachedMidiMap.mappingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        count = MIDI_MAPPINGS_MAX;
    }
    if (count == 0) {
        return;
    }
    for (uint32_t i = 0; i < count; ++i) {
        uint8_t note = cachedMidiMap.mappings[i].note;
        if (note == currentLitNote) {
            // This note is currently in playing state; don't clobber it.
            continue;
        }
        // Check per-sound override first, then fall back to global mappedVel.
        uint8_t vel = gl.mappedVel;
        const char *sfx = cachedMidiMap.mappings[i].sfxPath;
        for (uint32_t j = 0; j < cachedMidiMap.soundLightCount; ++j) {
            if (strcmp(cachedMidiMap.soundLights[j].sfxPath, sfx) == 0 &&
                cachedMidiMap.soundLights[j].hasMapped) {
                vel = cachedMidiMap.soundLights[j].mappedVel;
                break;
            }
        }
        sendLightNote(note, gl.channel, vel);
    }
}

void MidiContext::notifySoundStarted(const char *sfxBasename) {
    if (!sfxBasename || !sfxBasename[0]) {
        return;
    }
    const MidiLightGlobal &gl = cachedMidiMap.globalLight;
    // Find the MIDI note for this sound.
    uint32_t count = cachedMidiMap.mappingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        count = MIDI_MAPPINGS_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(cachedMidiMap.mappings[i].sfxPath, sfxBasename) != 0) {
            continue;
        }
        uint8_t note = cachedMidiMap.mappings[i].note;
        uint8_t vel = gl.playingVel;
        for (uint32_t j = 0; j < cachedMidiMap.soundLightCount; ++j) {
            if (strcmp(cachedMidiMap.soundLights[j].sfxPath, sfxBasename) == 0 &&
                cachedMidiMap.soundLights[j].hasPlaying) {
                vel = cachedMidiMap.soundLights[j].playingVel;
                break;
            }
        }
        // Return previously playing note to mapped state if different.
        if (currentLitNote != 255 && currentLitNote != note) {
            uint8_t prevVel = gl.mappedVel;
            sendLightNote(currentLitNote, gl.channel, prevVel);
        }
        currentLitNote = note;
        sendLightNote(note, gl.channel, vel);
        printf("[MIDI] LIGHT play note=%u vel=%u sfx=%s\n",
               (unsigned)note, (unsigned)vel, sfxBasename);
        return;
    }
    printf("[MIDI] LIGHT no mapping for sfx=%s (mappingCount=%u)\n",
           sfxBasename, (unsigned)count);
}

void MidiContext::notifySoundStopped() {
    if (currentLitNote == 255) {
        return;
    }
    const MidiLightGlobal &gl = cachedMidiMap.globalLight;
    uint8_t vel = gl.mappedVel;
    uint32_t count = cachedMidiMap.mappingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        count = MIDI_MAPPINGS_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (cachedMidiMap.mappings[i].note != currentLitNote) {
            continue;
        }
        const char *sfx = cachedMidiMap.mappings[i].sfxPath;
        for (uint32_t j = 0; j < cachedMidiMap.soundLightCount; ++j) {
            if (strcmp(cachedMidiMap.soundLights[j].sfxPath, sfx) == 0 &&
                cachedMidiMap.soundLights[j].hasMapped) {
                vel = cachedMidiMap.soundLights[j].mappedVel;
                break;
            }
        }
        break;
    }
    printf("[MIDI] LIGHT stop note=%u vel=%u\n", (unsigned)currentLitNote, (unsigned)vel);
    sendLightNote(currentLitNote, gl.channel, vel);
    currentLitNote = 255;
}

void MidiContext::poll() {
    if (!app || !app->config) {
        return;
    }

    uint64_t now = monotonicMillis();
    if (now >= nextMidiMapReloadMs) {
        cachedMidiMap = app->config->readMidiMapFile();
        nextMidiMapReloadMs = now + kMidiMapReloadIntervalMs;
    }

    // Check for MIDI lighting state changes from the audio subsystem.
    if (app->audio) {
        uint32_t trigSeq = app->audio->sfxTriggerSeq.load(std::memory_order_acquire);
        int nowPlaying = app->audio->sfxIsPlaying.load(std::memory_order_acquire);

        if (trigSeq != cachedTriggerSeq) {
            cachedTriggerSeq = trigSeq;
            char basename[256] = {};
            {
                std::lock_guard<std::mutex> lock(app->audio->sfxNameMutex);
                size_t n = strnlen(app->audio->sfxLastTriggeredBasename,
                                   sizeof(app->audio->sfxLastTriggeredBasename) - 1);
                memcpy(basename, app->audio->sfxLastTriggeredBasename, n);
                basename[n] = '\0';
            }
            notifySoundStarted(basename);
            // Force sfxWasPlaying=1 so if the clip ends before the next poll,
            // notifySoundStopped is still called to restore the LED.
            sfxWasPlaying = 1;
        } else if (sfxWasPlaying && !nowPlaying) {
            notifySoundStopped();
        }

        sfxWasPlaying = nowPlaying;
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
                if (kind != 0x90 || d1 == 0) {
                    continue;
                }

                ++lastNoteSeq;
                lastNote = d0;
                lastVelocity = d1;

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

                int triggerRc = app->audio->triggerSfx(sfxPath);
                if (triggerRc == RET_OK) {
                    printf("[MIDI] NOTE ON n=%u v=%u -> %s\n", (unsigned)d0, (unsigned)d1, sfxPath);
                } else {
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