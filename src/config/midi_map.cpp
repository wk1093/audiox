#include "config/context.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {

static inline char *trimLeft(char *s) {
    while (s && *s == ' ') {
        ++s;
    }
    return s;
}

static inline void trimRight(char *s) {
    if (!s) {
        return;
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == '\t')) {
        s[n - 1] = '\0';
        --n;
    }
}

static inline void copyBounded(char *dst, size_t dst_sz, const char *src) {
    if (!dst || dst_sz == 0) {
        return;
    }
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = strnlen(src, dst_sz - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static int parseMidiMapNoteLine(const char *value,
                                MidiMapData::MidiMapping *out,
                                uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_MAPPINGS_MAX) {
        return RET_ERR;
    }

    char buf[256];
    copyBounded(buf, sizeof(buf), value);

    char *comma = strchr(buf, ',');
    if (!comma) {
        return RET_ERR;
    }
    *comma = '\0';
    char *noteStr = trimLeft(buf);
    char *pathStr = trimLeft(comma + 1);
    trimRight(noteStr);
    trimRight(pathStr);

    char *endp = NULL;
    long note = strtol(noteStr, &endp, 10);
    if (!endp || *endp != '\0' || note < 0 || note > 127 || !pathStr[0]) {
        return RET_ERR;
    }

    out[*count_in_out].note = (uint8_t)note;
    copyBounded(out[*count_in_out].sfxPath, MIDI_SFX_PATH_MAX, pathStr);
    ++(*count_in_out);
    return RET_OK;
}

static int parseSoundLightLine(const char *value,
                               MidiSoundLight *out,
                               uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_SOUND_LIGHTS_MAX) {
        return RET_ERR;
    }

    char buf[320];
    copyBounded(buf, sizeof(buf), value);

    // format: sfxpath,mapped_vel,held_vel,playing_vel
    char *p = buf;
    char *sfxEnd = strchr(p, ',');
    if (!sfxEnd) {
        return RET_ERR;
    }
    *sfxEnd = '\0';
    char *sfxPath = trimLeft(p);
    trimRight(sfxPath);
    if (!sfxPath[0]) {
        return RET_ERR;
    }
    p = sfxEnd + 1;

    auto nextField = [&](char **fieldOut) -> int {
        char *comma = strchr(p, ',');
        const char *end = comma ? comma : p + strlen(p);
        char tmp[32];
        size_t len = (size_t)(end - p);
        if (len >= sizeof(tmp)) {
            return -1;
        }
        memcpy(tmp, p, len);
        tmp[len] = '\0';
        char *t = trimLeft(tmp);
        trimRight(t);
        char *ep = NULL;
        long v = strtol(t, &ep, 10);
        if (!ep || *ep != '\0' || v < 0 || v > 255) {
            return -1;
        }
        if (fieldOut) {
            *fieldOut = NULL;
        }
        if (comma) {
            p = comma + 1;
        } else {
            p = (char *)end;
        }
        (void)fieldOut;
        return (int)v;
    };

    int mVel = nextField(NULL);
    int hVel = nextField(NULL);
    int pVel = nextField(NULL);
    if (mVel < 0 || hVel < 0 || pVel < 0) {
        return RET_ERR;
    }

    MidiSoundLight *sl = &out[*count_in_out];
    copyBounded(sl->sfxPath, MIDI_SFX_PATH_MAX, sfxPath);
    sl->hasMapped = 1;
    sl->hasHeld = 1;
    sl->hasPlaying = 1;
    sl->mappedVel = (uint8_t)mVel;
    sl->heldVel = (uint8_t)hVel;
    sl->playingVel = (uint8_t)pVel;
    ++(*count_in_out);
    return RET_OK;
}

static int parseSoundModeLine(const char *value,
                              MidiSoundMode *out,
                              uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_SOUND_MODES_MAX) {
        return RET_ERR;
    }

    char buf[256];
    copyBounded(buf, sizeof(buf), value);

    char *comma = strchr(buf, ',');
    if (!comma) {
        return RET_ERR;
    }
    *comma = '\0';
    char *sfxPath = trimLeft(buf);
    char *modeStr = trimLeft(comma + 1);
    trimRight(sfxPath);
    trimRight(modeStr);

    if (!sfxPath[0]) {
        return RET_ERR;
    }

    uint8_t mode = soundboardModeFromString(modeStr);
    MidiSoundMode *sm = &out[*count_in_out];
    copyBounded(sm->sfxPath, MIDI_SFX_PATH_MAX, sfxPath);
    sm->mode = mode;
    ++(*count_in_out);
    return RET_OK;
}

static int readMidiMapPath(const char *path, MidiMapData *out) {
    if (!path || !out) {
        return RET_ERR;
    }

    MidiMapData data = {};
    // defaults: channel 0, all states off (vel=0)

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return (errno == ENOENT) ? RET_WARN : RET_ERR;
    }

    char line[320];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trimRight(key);
        trimRight(value);
        key = trimLeft(key);
        value = trimLeft(value);

        if (strcmp(key, "note_map") == 0) {
            (void)parseMidiMapNoteLine(value, data.mappings, &data.mappingCount);
        } else if (strcmp(key, "light_channel") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 15) {
                data.globalLight.channel = (uint8_t)v;
            }
        } else if (strcmp(key, "light_mapped_vel") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 127) {
                data.globalLight.mappedVel = (uint8_t)v;
            }
        } else if (strcmp(key, "light_held_vel") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 127) {
                data.globalLight.heldVel = (uint8_t)v;
            }
        } else if (strcmp(key, "light_playing_vel") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 127) {
                data.globalLight.playingVel = (uint8_t)v;
            }
        } else if (strcmp(key, "sound_light") == 0) {
            (void)parseSoundLightLine(value, data.soundLights, &data.soundLightCount);
        } else if (strcmp(key, "sound_mode") == 0) {
            (void)parseSoundModeLine(value, data.soundModes, &data.soundModeCount);
        }
    }

    if (ferror(fp)) {
        printf("[CONFIG] [WARN] failed while reading %s: %s\n", path, strerror(errno));
        fclose(fp);
        return RET_ERR;
    }
    fclose(fp);

    *out = data;
    return RET_OK;
}

// Migrate note_map entries from config.txt into a new midi_map.txt.
static void migrateMidiMapFromConfig(const char *configPath, MidiMapData *out) {
    if (!configPath || !out) {
        return;
    }

    FILE *fp = fopen(configPath, "r");
    if (!fp) {
        return;
    }

    char line[320];
    while (fgets(line, sizeof(line), fp)) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *value = eq + 1;
        trimRight(key);
        trimRight(value);
        key = trimLeft(key);
        value = trimLeft(value);

        if (strcmp(key, "midi_map") == 0) {
            (void)parseMidiMapNoteLine(value, out->mappings, &out->mappingCount);
        } else if (strncmp(key, "midi_note_", 10) == 0) {
            char mapLine[256];
            int n = snprintf(mapLine, sizeof(mapLine), "%s,%s", key + 10, value);
            if (n > 0 && (size_t)n < sizeof(mapLine)) {
                (void)parseMidiMapNoteLine(mapLine, out->mappings, &out->mappingCount);
            }
        }
    }

    fclose(fp);
}

static int writeMidiMapPath(const char *path, MidiMapData *data) {
    if (!path || !data) {
        return RET_ERR;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", path, strerror(errno));
        return RET_ERR;
    }

    dprintf(fd, "# audiox midi map v1\n");
    dprintf(fd, "# note-to-sfx mappings\n");
    uint32_t count = data->mappingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        count = MIDI_MAPPINGS_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!data->mappings[i].sfxPath[0]) {
            continue;
        }
        dprintf(fd, "note_map=%u,%s\n", (unsigned)data->mappings[i].note, data->mappings[i].sfxPath);
    }

    dprintf(fd, "# lighting config (velocities 0-127; channel 0-15)\n");
    dprintf(fd, "light_channel=%u\n", (unsigned)data->globalLight.channel);
    dprintf(fd, "light_mapped_vel=%u\n", (unsigned)data->globalLight.mappedVel);
    dprintf(fd, "light_held_vel=%u\n", (unsigned)data->globalLight.heldVel);
    dprintf(fd, "light_playing_vel=%u\n", (unsigned)data->globalLight.playingVel);

    uint32_t slCount = data->soundLightCount;
    if (slCount > MIDI_SOUND_LIGHTS_MAX) {
        slCount = MIDI_SOUND_LIGHTS_MAX;
    }
    if (slCount > 0) {
        dprintf(fd, "# per-sound lighting overrides: sfx,mapped_vel,held_vel,playing_vel\n");
        for (uint32_t i = 0; i < slCount; ++i) {
            if (!data->soundLights[i].sfxPath[0]) {
                continue;
            }
            dprintf(fd,
                    "sound_light=%s,%u,%u,%u\n",
                    data->soundLights[i].sfxPath,
                    (unsigned)data->soundLights[i].mappedVel,
                    (unsigned)data->soundLights[i].heldVel,
                    (unsigned)data->soundLights[i].playingVel);
        }
    }

    uint32_t smCount = data->soundModeCount;
    if (smCount > MIDI_SOUND_MODES_MAX) {
        smCount = MIDI_SOUND_MODES_MAX;
    }
    if (smCount > 0) {
        dprintf(fd, "# per-sound playback mode: sfx,mode(play|hold)\n");
        for (uint32_t i = 0; i < smCount; ++i) {
            if (!data->soundModes[i].sfxPath[0]) {
                continue;
            }
            dprintf(fd,
                    "sound_mode=%s,%s\n",
                    data->soundModes[i].sfxPath,
                    soundboardModeToString(data->soundModes[i].mode));
        }
    }

    close(fd);
    return RET_OK;
}

} // namespace

MidiMapData ConfigStore::readMidiMapFile() const {
    MidiMapData data = {};
    int rc = readMidiMapPath(MIDI_MAP_REAL_FILE_PATH, &data);
    if (rc == RET_WARN) {
        // File doesn't exist yet; migrate from config.txt.
        printf("[CONFIG] midi_map.txt not found, migrating from config.txt\n");
        migrateMidiMapFromConfig(CONFIG_REAL_FILE_PATH, &data);
        (void)writeMidiMapPath(MIDI_MAP_REAL_FILE_PATH, &data);
    }
    return data;
}

int ConfigStore::writeMidiMapFile(MidiMapData *data) {
    if (!data) {
        return RET_ERR;
    }
    uint32_t count = data->mappingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        data->mappingCount = MIDI_MAPPINGS_MAX;
    }
    uint32_t slCount = data->soundLightCount;
    if (slCount > MIDI_SOUND_LIGHTS_MAX) {
        data->soundLightCount = MIDI_SOUND_LIGHTS_MAX;
    }
    uint32_t smCount = data->soundModeCount;
    if (smCount > MIDI_SOUND_MODES_MAX) {
        data->soundModeCount = MIDI_SOUND_MODES_MAX;
    }
    return writeMidiMapPath(MIDI_MAP_REAL_FILE_PATH, data);
}
