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

    // format: sfxpath,mapped_vel,playing_vel
    // legacy format accepted: sfxpath,mapped_vel,held_vel,playing_vel
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
    int second = nextField(NULL);
    int third = nextField(NULL);
    if (mVel < 0 || second < 0) {
        return RET_ERR;
    }

    int pVel = second;
    if (third >= 0) {
        // Legacy 4-field format: third value is playing velocity.
        pVel = third;
    }

    MidiSoundLight *sl = &out[*count_in_out];
    copyBounded(sl->sfxPath, MIDI_SFX_PATH_MAX, sfxPath);
    sl->hasMapped = 1;
    sl->hasPlaying = 1;
    sl->mappedVel = (uint8_t)mVel;
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

static uint8_t midiActionFromString(const char *value) {
    if (!value) {
        return MIDI_ACTION_NONE;
    }
    if (strcmp(value, "stop_all") == 0) {
        return MIDI_ACTION_STOP_ALL;
    }
    if (strcmp(value, "sampler_toggle") == 0) {
        return MIDI_ACTION_SAMPLER_TOGGLE;
    }
    return MIDI_ACTION_NONE;
}

static const char *midiActionToString(uint8_t action) {
    switch (action) {
        case MIDI_ACTION_STOP_ALL:
            return "stop_all";
        case MIDI_ACTION_SAMPLER_TOGGLE:
            return "sampler_toggle";
        default:
            return NULL;
    }
}

static int parseCcVolumeLine(const char *value,
                             MidiCcVolumeMapping *out,
                             uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_CC_VOLUME_MAPPINGS_MAX) {
        return RET_ERR;
    }

    char buf[256];
    copyBounded(buf, sizeof(buf), value);

    char *comma = strchr(buf, ',');
    if (!comma) {
        return RET_ERR;
    }
    *comma = '\0';
    char *ccStr = trimLeft(buf);
    char *thingId = trimLeft(comma + 1);
    trimRight(ccStr);
    trimRight(thingId);

    char *endp = NULL;
    long cc = strtol(ccStr, &endp, 10);
    if (!endp || *endp != '\0' || cc < 0 || cc > 127 || !thingId[0]) {
        return RET_ERR;
    }
    if (strnlen(thingId, sizeof(out->thingId)) >= sizeof(out->thingId)) {
        return RET_ERR;
    }

    out[*count_in_out].cc = (uint8_t)cc;
    copyBounded(out[*count_in_out].thingId, sizeof(out[*count_in_out].thingId), thingId);
    ++(*count_in_out);
    return RET_OK;
}

static int parseEffectStateLine(const char *value,
                                MidiEffectState *out,
                                uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_EFFECT_STATES_MAX) {
        return RET_ERR;
    }

    char buf[320];
    copyBounded(buf, sizeof(buf), value);

    // format: effect_id,type,enabled,gain,drive,clip,output
    char *parts[7] = {};
    size_t partCount = 0;
    char *cursor = buf;
    while (partCount < 7) {
        parts[partCount++] = cursor;
        char *comma = strchr(cursor, ',');
        if (!comma) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    if (partCount != 7 || strchr(parts[6], ',') != NULL) {
        return RET_ERR;
    }

    for (size_t i = 0; i < 7; ++i) {
        parts[i] = trimLeft(parts[i]);
        trimRight(parts[i]);
    }

    if (!parts[0][0] || !parts[1][0] || !parts[2][0]) {
        return RET_ERR;
    }

    char *ep = NULL;
    long enabled = strtol(parts[2], &ep, 10);
    if (!ep || *ep != '\0' || (enabled != 0 && enabled != 1)) {
        return RET_ERR;
    }

    ep = NULL;
    float gain = strtof(parts[3], &ep);
    if (!ep || *ep != '\0') {
        return RET_ERR;
    }
    ep = NULL;
    float drive = strtof(parts[4], &ep);
    if (!ep || *ep != '\0') {
        return RET_ERR;
    }
    ep = NULL;
    float clip = strtof(parts[5], &ep);
    if (!ep || *ep != '\0') {
        return RET_ERR;
    }
    ep = NULL;
    float output = strtof(parts[6], &ep);
    if (!ep || *ep != '\0') {
        return RET_ERR;
    }

    if (strncmp(parts[0], "fx_", 3) != 0) {
        return RET_ERR;
    }
    if (strcmp(parts[1], "gain") != 0 && strcmp(parts[1], "distortion") != 0) {
        return RET_ERR;
    }

    MidiEffectState *state = &out[*count_in_out];
    memset(state, 0, sizeof(*state));
    copyBounded(state->effectId, sizeof(state->effectId), parts[0]);
    copyBounded(state->type, sizeof(state->type), parts[1]);
    state->enabled = (enabled != 0) ? 1U : 0U;
    state->gain = gain;
    state->drive = drive;
    state->clip = clip;
    state->output = output;
    ++(*count_in_out);
    return RET_OK;
}

static int parseEffectCcLine(const char *value,
                             MidiEffectCcMapping *out,
                             uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_EFFECT_CC_MAPPINGS_MAX) {
        return RET_ERR;
    }

    char buf[256];
    copyBounded(buf, sizeof(buf), value);

    // format: cc,effect_id,param
    char *parts[3] = {};
    size_t partCount = 0;
    char *cursor = buf;
    while (partCount < 3) {
        parts[partCount++] = cursor;
        char *comma = strchr(cursor, ',');
        if (!comma) {
            break;
        }
        *comma = '\0';
        cursor = comma + 1;
    }
    if (partCount != 3 || strchr(parts[2], ',') != NULL) {
        return RET_ERR;
    }

    for (size_t i = 0; i < 3; ++i) {
        parts[i] = trimLeft(parts[i]);
        trimRight(parts[i]);
    }

    char *ep = NULL;
    long cc = strtol(parts[0], &ep, 10);
    if (!ep || *ep != '\0' || cc < 0 || cc > 127) {
        return RET_ERR;
    }
    if (!parts[1][0] || strncmp(parts[1], "fx_", 3) != 0) {
        return RET_ERR;
    }
    if (strcmp(parts[2], "gain") != 0 &&
        strcmp(parts[2], "drive") != 0 &&
        strcmp(parts[2], "clip") != 0 &&
        strcmp(parts[2], "output") != 0) {
        return RET_ERR;
    }

    MidiEffectCcMapping *map = &out[*count_in_out];
    memset(map, 0, sizeof(*map));
    map->cc = (uint8_t)cc;
    copyBounded(map->effectId, sizeof(map->effectId), parts[1]);
    copyBounded(map->param, sizeof(map->param), parts[2]);
    ++(*count_in_out);
    return RET_OK;
}

static int parseEffectToggleLine(const char *value,
                                 MidiEffectToggleMapping *out,
                                 uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_EFFECT_TOGGLE_MAPPINGS_MAX) {
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
    char *effectId = trimLeft(comma + 1);
    trimRight(noteStr);
    trimRight(effectId);

    char *ep = NULL;
    long note = strtol(noteStr, &ep, 10);
    if (!ep || *ep != '\0' || note < 0 || note > 127) {
        return RET_ERR;
    }
    if (!effectId[0] || strncmp(effectId, "fx_", 3) != 0) {
        return RET_ERR;
    }

    MidiEffectToggleMapping *map = &out[*count_in_out];
    memset(map, 0, sizeof(*map));
    map->note = (uint8_t)note;
    copyBounded(map->effectId, sizeof(map->effectId), effectId);
    ++(*count_in_out);
    return RET_OK;
}

static int parseMidiActionLine(const char *value,
                               MidiActionMapping *out,
                               uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_ACTION_MAPPINGS_MAX) {
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
    char *actionStr = trimLeft(comma + 1);
    trimRight(noteStr);
    trimRight(actionStr);

    char *endp = NULL;
    long note = strtol(noteStr, &endp, 10);
    if (!endp || *endp != '\0' || note < 0 || note > 127) {
        return RET_ERR;
    }

    uint8_t action = midiActionFromString(actionStr);
    if (action == MIDI_ACTION_NONE) {
        return RET_ERR;
    }

    out[*count_in_out].note = (uint8_t)note;
    out[*count_in_out].action = action;
    ++(*count_in_out);
    return RET_OK;
}

static int readMidiMapPath(const char *path, MidiMapData *out) {
    if (!path || !out) {
        return RET_ERR;
    }

    MidiMapData data = {};
    data.samplerKeyboardEnabled = 0;
    data.samplerKeyboardChannel = 0;
    data.samplerRootNote = 60;
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
        } else if (strcmp(key, "light_playing_vel") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 127) {
                data.globalLight.playingVel = (uint8_t)v;
            }
        } else if (strcmp(key, "light_stop_all_vel") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 127) {
                data.globalLight.stopAllVel = (uint8_t)v;
            }
        } else if (strcmp(key, "light_held_vel") == 0) {
            // Legacy key: if present, treat it as stop-all color during migration.
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 127) {
                data.globalLight.stopAllVel = (uint8_t)v;
            }
        } else if (strcmp(key, "sound_light") == 0) {
            (void)parseSoundLightLine(value, data.soundLights, &data.soundLightCount);
        } else if (strcmp(key, "sound_mode") == 0) {
            (void)parseSoundModeLine(value, data.soundModes, &data.soundModeCount);
        } else if (strcmp(key, "note_action") == 0) {
            (void)parseMidiActionLine(value, data.actionMappings, &data.actionMappingCount);
        } else if (strcmp(key, "sampler_keyboard_channel") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 15) {
                data.samplerKeyboardEnabled = 1;
                data.samplerKeyboardChannel = (uint8_t)v;
            }
        } else if (strcmp(key, "sampler_keyboard_enabled") == 0) {
            long v = strtol(value, NULL, 10);
            data.samplerKeyboardEnabled = (v != 0) ? 1 : 0;
        } else if (strcmp(key, "sampler_root_note") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 127) {
                data.samplerRootNote = (uint8_t)v;
            }
        } else if (strcmp(key, "cc_volume") == 0) {
            (void)parseCcVolumeLine(value, data.ccVolumeMappings, &data.ccVolumeMappingCount);
        } else if (strcmp(key, "fx_state") == 0) {
            (void)parseEffectStateLine(value, data.effectStates, &data.effectStateCount);
        } else if (strcmp(key, "fx_cc") == 0) {
            (void)parseEffectCcLine(value, data.effectCcMappings, &data.effectCcMappingCount);
        } else if (strcmp(key, "fx_toggle") == 0) {
            (void)parseEffectToggleLine(value, data.effectToggleMappings, &data.effectToggleMappingCount);
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
    dprintf(fd, "light_playing_vel=%u\n", (unsigned)data->globalLight.playingVel);
    dprintf(fd, "light_stop_all_vel=%u\n", (unsigned)data->globalLight.stopAllVel);

    uint32_t slCount = data->soundLightCount;
    if (slCount > MIDI_SOUND_LIGHTS_MAX) {
        slCount = MIDI_SOUND_LIGHTS_MAX;
    }
    if (slCount > 0) {
        dprintf(fd, "# per-sound lighting overrides: sfx,mapped_vel,playing_vel\n");
        for (uint32_t i = 0; i < slCount; ++i) {
            if (!data->soundLights[i].sfxPath[0]) {
                continue;
            }
            dprintf(fd,
                    "sound_light=%s,%u,%u\n",
                    data->soundLights[i].sfxPath,
                    (unsigned)data->soundLights[i].mappedVel,
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

    uint32_t actionCount = data->actionMappingCount;
    if (actionCount > MIDI_ACTION_MAPPINGS_MAX) {
        actionCount = MIDI_ACTION_MAPPINGS_MAX;
    }
    if (actionCount > 0) {
        dprintf(fd, "# action mappings: note,action(stop_all|sampler_toggle)\n");
        for (uint32_t i = 0; i < actionCount; ++i) {
            const char *action = midiActionToString(data->actionMappings[i].action);
            if (!action) {
                continue;
            }
            dprintf(fd,
                    "note_action=%u,%s\n",
                    (unsigned)data->actionMappings[i].note,
                    action);
        }
    }

    dprintf(fd, "# sampler keyboard config\n");
    dprintf(fd, "sampler_keyboard_enabled=%u\n", (unsigned)data->samplerKeyboardEnabled);
    dprintf(fd, "sampler_keyboard_channel=%u\n", (unsigned)data->samplerKeyboardChannel);
    dprintf(fd, "sampler_root_note=%u\n", (unsigned)data->samplerRootNote);

    uint32_t cvCount = data->ccVolumeMappingCount;
    if (cvCount > MIDI_CC_VOLUME_MAPPINGS_MAX) {
        cvCount = MIDI_CC_VOLUME_MAPPINGS_MAX;
    }
    if (cvCount > 0) {
        dprintf(fd, "# MIDI CC to volume mappings: cc,thing_id\n");
        for (uint32_t i = 0; i < cvCount; ++i) {
            if (!data->ccVolumeMappings[i].thingId[0]) {
                continue;
            }
            dprintf(fd,
                    "cc_volume=%u,%s\n",
                    (unsigned)data->ccVolumeMappings[i].cc,
                    data->ccVolumeMappings[i].thingId);
        }
    }

    uint32_t fxStateCount = data->effectStateCount;
    if (fxStateCount > MIDI_EFFECT_STATES_MAX) {
        fxStateCount = MIDI_EFFECT_STATES_MAX;
    }
    if (fxStateCount > 0) {
        dprintf(fd, "# effect state: effect_id,type,enabled,gain,drive,clip,output\n");
        for (uint32_t i = 0; i < fxStateCount; ++i) {
            if (!data->effectStates[i].effectId[0]) {
                continue;
            }
            dprintf(fd,
                    "fx_state=%s,%s,%u,%.6f,%.6f,%.6f,%.6f\n",
                    data->effectStates[i].effectId,
                    data->effectStates[i].type[0] ? data->effectStates[i].type : "gain",
                    (unsigned)(data->effectStates[i].enabled ? 1U : 0U),
                    (double)data->effectStates[i].gain,
                    (double)data->effectStates[i].drive,
                    (double)data->effectStates[i].clip,
                    (double)data->effectStates[i].output);
        }
    }

    uint32_t fxCcCount = data->effectCcMappingCount;
    if (fxCcCount > MIDI_EFFECT_CC_MAPPINGS_MAX) {
        fxCcCount = MIDI_EFFECT_CC_MAPPINGS_MAX;
    }
    if (fxCcCount > 0) {
        dprintf(fd, "# effect MIDI CC mappings: cc,effect_id,param\n");
        for (uint32_t i = 0; i < fxCcCount; ++i) {
            if (!data->effectCcMappings[i].effectId[0] || !data->effectCcMappings[i].param[0]) {
                continue;
            }
            dprintf(fd,
                    "fx_cc=%u,%s,%s\n",
                    (unsigned)data->effectCcMappings[i].cc,
                    data->effectCcMappings[i].effectId,
                    data->effectCcMappings[i].param);
        }
    }

    uint32_t fxToggleCount = data->effectToggleMappingCount;
    if (fxToggleCount > MIDI_EFFECT_TOGGLE_MAPPINGS_MAX) {
        fxToggleCount = MIDI_EFFECT_TOGGLE_MAPPINGS_MAX;
    }
    if (fxToggleCount > 0) {
        dprintf(fd, "# effect bypass toggle mappings: note,effect_id\n");
        for (uint32_t i = 0; i < fxToggleCount; ++i) {
            if (!data->effectToggleMappings[i].effectId[0]) {
                continue;
            }
            dprintf(fd,
                    "fx_toggle=%u,%s\n",
                    (unsigned)data->effectToggleMappings[i].note,
                    data->effectToggleMappings[i].effectId);
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
    uint32_t actionCount = data->actionMappingCount;
    if (actionCount > MIDI_ACTION_MAPPINGS_MAX) {
        data->actionMappingCount = MIDI_ACTION_MAPPINGS_MAX;
    }
    uint32_t cvCount = data->ccVolumeMappingCount;
    if (cvCount > MIDI_CC_VOLUME_MAPPINGS_MAX) {
        data->ccVolumeMappingCount = MIDI_CC_VOLUME_MAPPINGS_MAX;
    }
    uint32_t fxStateCount = data->effectStateCount;
    if (fxStateCount > MIDI_EFFECT_STATES_MAX) {
        data->effectStateCount = MIDI_EFFECT_STATES_MAX;
    }
    uint32_t fxCcCount = data->effectCcMappingCount;
    if (fxCcCount > MIDI_EFFECT_CC_MAPPINGS_MAX) {
        data->effectCcMappingCount = MIDI_EFFECT_CC_MAPPINGS_MAX;
    }
    uint32_t fxToggleCount = data->effectToggleMappingCount;
    if (fxToggleCount > MIDI_EFFECT_TOGGLE_MAPPINGS_MAX) {
        data->effectToggleMappingCount = MIDI_EFFECT_TOGGLE_MAPPINGS_MAX;
    }
    return writeMidiMapPath(MIDI_MAP_REAL_FILE_PATH, data);
}
