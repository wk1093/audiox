#include "config/context.hpp"
#include "config/file.hpp"
#include "audio/plugin_registry.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace {

static inline char *trimLeft(char *s) {
    while (s && (*s == ' ' || *s == '\t')) {
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

static int parseSoundboardGridCellLine(const char *value,
                                       SoundboardGridCell *out,
                                       uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= SOUNDBOARD_GRID_CELLS_MAX) {
        return RET_ERR;
    }

    char sfxPath[MIDI_SFX_PATH_MAX]{};
    unsigned row = 0;
    unsigned column = 0;
    char extra = '\0';
    if (sscanf(value, "%159[^,],%u,%u%c", sfxPath, &row, &column, &extra) != 3 ||
        !sfxPath[0] || row >= SOUNDBOARD_GRID_ROWS_MAX || column >= SOUNDBOARD_GRID_COLUMNS_MAX) {
        return RET_ERR;
    }

    SoundboardGridCell &cell = out[(*count_in_out)++];
    copyBounded(cell.sfxPath, sizeof(cell.sfxPath), sfxPath);
    cell.row = (uint8_t)row;
    cell.column = (uint8_t)column;
    return RET_OK;
}

static int parseSoundboardGridCornerLine(const char *value, MidiMapData *data) {
    if (!value || !data) return RET_ERR;
    char cornerName[24]{};
    unsigned note = 0;
    char extra = '\0';
    if (sscanf(value, "%23[^,],%u%c", cornerName, &note, &extra) != 2 || note > 127) {
        return RET_ERR;
    }
    static const char *const names[SOUNDBOARD_GRID_CORNER_COUNT] = {
        "top_left", "top_right", "bottom_left", "bottom_right",
    };
    for (uint8_t i = 0; i < SOUNDBOARD_GRID_CORNER_COUNT; ++i) {
        if (strcmp(cornerName, names[i]) == 0) {
            data->gridCornerNotes[i] = (uint8_t)note;
            data->gridCornerMask |= (uint8_t)(1U << i);
            return RET_OK;
        }
    }
    return RET_ERR;
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
    if (!ep || ep == parts[3] || *ep != '\0' || !std::isfinite(gain)) {
        return RET_ERR;
    }
    ep = NULL;
    float drive = strtof(parts[4], &ep);
    if (!ep || ep == parts[4] || *ep != '\0' || !std::isfinite(drive)) {
        return RET_ERR;
    }
    ep = NULL;
    float clip = strtof(parts[5], &ep);
    if (!ep || ep == parts[5] || *ep != '\0' || !std::isfinite(clip)) {
        return RET_ERR;
    }
    ep = NULL;
    float output = strtof(parts[6], &ep);
    if (!ep || ep == parts[6] || *ep != '\0' || !std::isfinite(output)) {
        return RET_ERR;
    }

    if (strncmp(parts[0], "fx_", 3) != 0) {
        return RET_ERR;
    }
    if (strcmp(parts[1], "gain") != 0 &&
        strcmp(parts[1], "distortion") != 0 &&
        strcmp(parts[1], "pitch") != 0 &&
        strcmp(parts[1], "reverb") != 0 &&
        strcmp(parts[1], "gate") != 0 &&
        strcmp(parts[1], "cut") != 0 &&
        strcmp(parts[1], "compressor") != 0 &&
        strcmp(parts[1], "limiter") != 0 &&
        strcmp(parts[1], "eq") != 0 &&
        strcmp(parts[1], "multiband") != 0 &&
        strcmp(parts[1], "noise_reduction") != 0 && !audiox::plugin::validTypeName(parts[1])) {
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
    for (char *p = parts[2]; *p; ++p) {
        if (((*p >= 'a' && *p <= 'z') ||
             (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9') ||
             *p == '_') == 0) {
            return RET_ERR;
        }
    }

    MidiEffectCcMapping *map = &out[*count_in_out];
    memset(map, 0, sizeof(*map));
    map->cc = (uint8_t)cc;
    copyBounded(map->effectId, sizeof(map->effectId), parts[1]);
    copyBounded(map->param, sizeof(map->param), parts[2]);
    ++(*count_in_out);
    return RET_OK;
}

static int parseEffectParamLine(const char *value,
                                MidiEffectParam *out,
                                uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_EFFECT_PARAMS_MAX) {
        return RET_ERR;
    }

    char buf[256];
    copyBounded(buf, sizeof(buf), value);

    // format: effect_id,param,value
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

    if (!parts[0][0] || strncmp(parts[0], "fx_", 3) != 0 || !parts[1][0]) {
        return RET_ERR;
    }
    for (char *p = parts[1]; *p; ++p) {
        if (((*p >= 'a' && *p <= 'z') ||
             (*p >= 'A' && *p <= 'Z') ||
             (*p >= '0' && *p <= '9') ||
             *p == '_') == 0) {
            return RET_ERR;
        }
    }

    char *ep = NULL;
    float parsed = strtof(parts[2], &ep);
    if (!ep || ep == parts[2] || *ep != '\0' || !std::isfinite(parsed)) {
        return RET_ERR;
    }

    MidiEffectParam *entry = &out[*count_in_out];
    memset(entry, 0, sizeof(*entry));
    copyBounded(entry->effectId, sizeof(entry->effectId), parts[0]);
    copyBounded(entry->param, sizeof(entry->param), parts[1]);
    entry->value = parsed;
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

static int parseEffectLightLine(const char *value,
                                MidiEffectLightMapping *out,
                                uint32_t *count_in_out) {
    if (!value || !out || !count_in_out || *count_in_out >= MIDI_EFFECT_LIGHT_MAPPINGS_MAX) {
        return RET_ERR;
    }

    char buf[320];
    copyBounded(buf, sizeof(buf), value);

    // format: effect_id,enabled_vel,bypassed_vel
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

    if (!parts[0][0] || strncmp(parts[0], "fx_", 3) != 0) {
        return RET_ERR;
    }

    char *ep = NULL;
    long enabledVel = strtol(parts[1], &ep, 10);
    if (!ep || *ep != '\0' || enabledVel < 0 || enabledVel > 127) {
        return RET_ERR;
    }
    ep = NULL;
    long bypassedVel = strtol(parts[2], &ep, 10);
    if (!ep || *ep != '\0' || bypassedVel < 0 || bypassedVel > 127) {
        return RET_ERR;
    }

    MidiEffectLightMapping *map = &out[*count_in_out];
    memset(map, 0, sizeof(*map));
    copyBounded(map->effectId, sizeof(map->effectId), parts[0]);
    map->enabledVel = (uint8_t)enabledVel;
    map->bypassedVel = (uint8_t)bypassedVel;
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

static void assignMacroIdentities(MidiMapData &data) {
    for (uint32_t i = 0; i < data.chordCount; ++i) {
        auto &macro = data.chords[i];
        if (!macro.id) {
            uint32_t candidate = 1;
            for (;;) {
                bool used = false;
                for (uint32_t j = 0; j < data.chordCount; ++j)
                    if (data.chords[j].id == candidate) used = true;
                if (!used) break;
                ++candidate;
            }
            macro.id = candidate;
        }
        if (!macro.name[0])
            snprintf(macro.name, sizeof(macro.name), "MIDI %u:%u", macro.channel, macro.note);
    }
}

static int readMidiMapPath(const char *path, MidiMapData *out, bool scriptsOnly = false) {
    if (!path || !out) {
        return RET_ERR;
    }

    MidiMapData data = {};
    data.samplerKeyboardEnabled = 0;
    bool explicitSamplerEnabled = false;
    data.samplerKeyboardChannel = 0;
    data.samplerRootNote = 60;
    data.gridRows = 4;
    data.gridColumns = 4;
    // defaults: channel 0, all states off (vel=0)

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return (errno == ENOENT) ? RET_WARN : RET_ERR;
    }
    ConfigTextSchemaStatus schema =
        checkConfigTextSchema(fp, scriptsOnly ? "# audiox scripts v" : "# audiox midi map v",
                              scriptsOnly ? SCRIPTS_SCHEMA_VERSION : MIDI_MAP_SCHEMA_VERSION);
    if (schema == ConfigTextSchemaStatus::Unsupported ||
        schema == ConfigTextSchemaStatus::Invalid ||
        schema == ConfigTextSchemaStatus::IoError) {
        printf("[CONFIG] [WARN] unsupported or invalid MIDI map schema in %s\n", path);
        fclose(fp);
        return RET_ERR;
    }

    char line[SCRIPT_STORAGE_MAX * 2 + 384];
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
        } else if (strcmp(key, "sound_order") == 0) {
            if (value[0] && strlen(value) < MIDI_SFX_PATH_MAX && data.soundOrderCount < SOUNDBOARD_ORDER_MAX) {
                copyBounded(data.soundOrder[data.soundOrderCount++].sfxPath, MIDI_SFX_PATH_MAX, value);
            }
        } else if (strcmp(key, "soundboard_layout") == 0) {
            char mode[16]{};
            unsigned rows = 0;
            unsigned columns = 0;
            char extra = '\0';
            if (sscanf(value, "%15[^,],%u,%u%c", mode, &rows, &columns, &extra) == 3 &&
                rows >= 1 && rows <= SOUNDBOARD_GRID_ROWS_MAX &&
                columns >= 1 && columns <= SOUNDBOARD_GRID_COLUMNS_MAX &&
                rows * columns <= SOUNDBOARD_GRID_CELLS_MAX) {
                if (strcmp(mode, "normal") == 0 || strcmp(mode, "grid") == 0) {
                    data.soundboardLayout = strcmp(mode, "grid") == 0
                        ? SOUNDBOARD_LAYOUT_GRID : SOUNDBOARD_LAYOUT_NORMAL;
                    data.gridRows = (uint8_t)rows;
                    data.gridColumns = (uint8_t)columns;
                }
            }
        } else if (strcmp(key, "soundboard_grid_corner") == 0) {
            (void)parseSoundboardGridCornerLine(value, &data);
        } else if (strcmp(key, "soundboard_grid_initialized") == 0) {
            data.gridInitialized = strcmp(value, "1") == 0;
        } else if (strcmp(key, "soundboard_grid_cell") == 0) {
            (void)parseSoundboardGridCellLine(value, data.gridCells, &data.gridCellCount);
        } else if (strcmp(key, "note_action") == 0) {
            (void)parseMidiActionLine(value, data.actionMappings, &data.actionMappingCount);
        } else if (strcmp(key, "automation") == 0) {
            unsigned id, enabled, channel, note, idle, pressed;
            char nameHex[128]{}, keys[64]{};
            int offset = 0;
            ScriptDefinition chord{};
            if (data.chordCount < SCRIPTS_MAX &&
                sscanf(value, "%u,%u,%u,%u,%u,%u,%127[^,],%63[^,],%n",
                       &id, &enabled, &channel, &note, &idle, &pressed, nameHex, keys, &offset) == 8 &&
                offset > 0 && id > 0 && enabled <= 1 && channel <= 15 && note <= 127 &&
                idle <= 127 && pressed <= 127 &&
                scriptHexDecode(nameHex, chord.name, sizeof(chord.name)) && chord.name[0] &&
                scriptHexDecode(value + offset, chord.script, sizeof(chord.script))) {
                chord.id = id;
                chord.midiEnabled = enabled != 0;
                chord.channel = channel;
                chord.note = note;
                chord.idleVelocity = idle;
                chord.pressedVelocity = pressed;
                if (strcmp(keys, "-")) strcpy(chord.keys, keys);
                uint8_t report[8];
                bool valid = (chord.script[0] || chord.keys[0]) &&
                    (!chord.keys[0] || scriptChordReport(chord.keys, report));
                for (uint32_t j = 0; j < data.chordCount; ++j)
                    if (data.chords[j].id == chord.id) valid = false;
                if (valid) {
                    data.chords[data.chordCount++] = chord;
                    continue;
                }
            }
            printf("[CONFIG] [WARN] Invalid or excess automation entry\n");
        } else if (strcmp(key, "automation_events") == 0 ||
                   strcmp(key, "automation_config_write") == 0 ||
                   strcmp(key, "automation_hooks") == 0) {
            unsigned id = 0, mask = 0;
            char extra = '\0';
            bool found = false;
            bool writePermission = strcmp(key, "automation_config_write") == 0;
            bool hookMode = strcmp(key, "automation_hooks") == 0;
            if (sscanf(value, "%u,%u%c", &id, &mask, &extra) == 2 &&
                mask <= (writePermission || hookMode ? 1 : SCRIPT_EVENTS_MASK)) {
                for (uint32_t j = 0; j < data.chordCount; ++j) {
                    if (data.chords[j].id == id) {
                        if (writePermission) data.chords[j].configWriteAllowed = mask != 0;
                        else if (hookMode) data.chords[j].hookMode = mask != 0;
                        else data.chords[j].eventMask = static_cast<uint16_t>(mask);
                        found = true;
                    }
                }
            }
            if (!found) printf("[CONFIG] [WARN] Invalid %s entry (must follow its automation)\n", key);
        } else if (strcmp(key, "key_macro") == 0) {
            unsigned channel, note, idle, pressed;
            char keys[64]{};
            int offset = 0;
            if (data.chordCount < SCRIPTS_MAX &&
                sscanf(value, "%u,%u,%u,%u,%63[^,],%n", &channel, &note, &idle, &pressed, keys, &offset) == 5 &&
                offset > 0 && channel <= 15 && note <= 127 && idle <= 127 && pressed <= 127 &&
                strlen(value + offset) < SCRIPT_MAX * 2) {
                ScriptDefinition chord{};
                chord.channel = channel;
                chord.note = note;
                chord.idleVelocity = idle;
                chord.pressedVelocity = pressed;
                if (strcmp(keys, "-")) strcpy(chord.keys, keys);
                uint8_t report[8];
                if (scriptHexDecode(value + offset, chord.script, sizeof(chord.script)) &&
                    (!chord.keys[0] || scriptChordReport(chord.keys, report)) &&
                    (chord.script[0] || chord.keys[0])) {
                    data.chords[data.chordCount++] = chord;
                    continue;
                }
            }
            printf("[CONFIG] [WARN] Invalid or excess key_macro entry\n");
        } else if (strcmp(key, "key_chord") == 0) {
            unsigned channel, note;
            char keys[64]{}, extra;
            uint8_t report[8];
            if (data.chordCount < SCRIPTS_MAX &&
                sscanf(value, "%u,%u,%63[^,\r\n]%c", &channel, &note, keys, &extra) == 3 &&
                channel <= 15 && note <= 127 && scriptChordReport(keys, report)) {
                ScriptDefinition &chord = data.chords[data.chordCount++];
                chord.channel = channel;
                chord.note = note;
                strcpy(chord.keys, keys);
            } else {
                printf("[CONFIG] [WARN] Invalid or excess key_chord entry\n");
            }
        } else if (strcmp(key, "sampler_keyboard_channel") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 15) {
                if (!explicitSamplerEnabled) data.samplerKeyboardEnabled = 1;
                data.samplerKeyboardChannel = (uint8_t)v;
            }
        } else if (strcmp(key, "sampler_keyboard_enabled") == 0) {
            long v = strtol(value, NULL, 10);
            data.samplerKeyboardEnabled = (v != 0) ? 1 : 0;
            explicitSamplerEnabled = true;
        } else if (strcmp(key, "sampler_root_note") == 0) {
            long v = strtol(value, NULL, 10);
            if (v >= 0 && v <= 127) {
                data.samplerRootNote = (uint8_t)v;
            }
        } else if (strcmp(key, "cc_volume") == 0) {
            (void)parseCcVolumeLine(value, data.ccVolumeMappings, &data.ccVolumeMappingCount);
        } else if (strcmp(key, "fx_state") == 0) {
            if (parseEffectStateLine(value, data.effectStates, &data.effectStateCount) != RET_OK) {
                printf("[CONFIG] [WARN] invalid fx_state in MIDI config\n");
            }
        } else if (strcmp(key, "fx_name") == 0) {
            char *comma = strchr(value, ',');
            bool valid = false;
            if (comma) {
                *comma = '\0';
                char name[64]{};
                if (scriptHexDecode(comma + 1, name, sizeof(name)) && midiEffectNameValid(name)) {
                    for (uint32_t i = 0; i < data.effectStateCount; ++i) {
                        if (!strcmp(data.effectStates[i].effectId, value)) {
                            copyBounded(data.effectStates[i].name, sizeof(data.effectStates[i].name), name);
                            valid = true;
                            break;
                        }
                    }
                }
            }
            if (!valid) printf("[CONFIG] [WARN] invalid fx_name (must follow its fx_state)\n");
        } else if (strcmp(key, "fx_param") == 0) {
            if (parseEffectParamLine(value, data.effectParams, &data.effectParamCount) != RET_OK) {
                printf("[CONFIG] [WARN] invalid fx_param in MIDI config\n");
            }
        } else if (strcmp(key, "fx_cc") == 0) {
            (void)parseEffectCcLine(value, data.effectCcMappings, &data.effectCcMappingCount);
        } else if (strcmp(key, "fx_toggle") == 0) {
            (void)parseEffectToggleLine(value, data.effectToggleMappings, &data.effectToggleMappingCount);
        } else if (strcmp(key, "fx_light") == 0) {
            (void)parseEffectLightLine(value, data.effectLightMappings, &data.effectLightMappingCount);
        }
    }

    if (ferror(fp)) {
        printf("[CONFIG] [WARN] failed while reading %s: %s\n", path, strerror(errno));
        fclose(fp);
        return RET_ERR;
    }
    fclose(fp);

    assignMacroIdentities(data);
    for (uint32_t i = 0; i < data.chordCount; ++i) {
        char error[192]{};
        if (!scriptMigrate(data.chords[i], error, sizeof(error)))
            printf("[CONFIG] [WARN] Cannot convert script %u: %s\n", data.chords[i].id, error);
    }
    if (data.soundboardLayout == SOUNDBOARD_LAYOUT_GRID || data.gridCellCount > 0)
        data.gridInitialized = 1;
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

static int writeMidiMapPath(const char *path, MidiMapData *data, bool scriptsOnly = false) {
    if (!path || !data) {
        return RET_ERR;
    }
    ConfigTextSchemaStatus schema =
        checkConfigTextFileSchema(path, scriptsOnly ? "# audiox scripts v" : "# audiox midi map v",
                                  scriptsOnly ? SCRIPTS_SCHEMA_VERSION : MIDI_MAP_SCHEMA_VERSION);
    if (schema == ConfigTextSchemaStatus::Unsupported ||
        schema == ConfigTextSchemaStatus::Invalid ||
        schema == ConfigTextSchemaStatus::IoError) {
        printf("[CONFIG] [WARN] refusing to overwrite unsupported MIDI map schema in %s\n", path);
        return RET_ERR;
    }

    audiox::AtomicConfigFile output(path);
    FILE *fd = output.get();
    if (!fd) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", path, strerror(errno));
        return RET_ERR;
    }

    if (fprintf(fd, scriptsOnly ? "# audiox scripts v%u\n" : "# audiox midi map v%u\n",
                scriptsOnly ? SCRIPTS_SCHEMA_VERSION : MIDI_MAP_SCHEMA_VERSION) < 0) {
        printf("[CONFIG] [WARN] failed to write MIDI map schema in %s: %s\n", path, strerror(errno));
        return RET_ERR;
    }
    for (uint32_t i = 0; scriptsOnly && i < data->chordCount && i < SCRIPTS_MAX; ++i) {
        const auto &chord = data->chords[i];
        std::string hex = scriptHexEncode(chord.script);
        if (fprintf(fd, "automation=%u,%u,%u,%u,%u,%u,%s,%s,%s\n",
                    chord.id, chord.midiEnabled ? 1 : 0, chord.channel, chord.note,
                    chord.idleVelocity, chord.pressedVelocity, scriptHexEncode(chord.name).c_str(),
                    chord.keys[0] ? chord.keys : "-", hex.c_str()) < 0 ||
            fprintf(fd, "automation_events=%u,%u\n", chord.id, chord.eventMask) < 0 ||
            fprintf(fd, "automation_config_write=%u,%u\n", chord.id, chord.configWriteAllowed ? 1 : 0) < 0 ||
            fprintf(fd, "automation_hooks=%u,%u\n", chord.id, chord.hookMode ? 1 : 0) < 0) {
            printf("[CONFIG] [WARN] Cannot write macro mapping\n");
            return RET_ERR;
        }
    }
    if (scriptsOnly) {
        if (output.commit() == 0) return RET_OK;
        printf("[CONFIG] [WARN] failed to save scripts: %s\n", strerror(errno));
        return RET_ERR;
    }
    fprintf(fd, "# note-to-sfx mappings\n");
    uint32_t count = data->mappingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        count = MIDI_MAPPINGS_MAX;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!data->mappings[i].sfxPath[0]) {
            continue;
        }
        fprintf(fd, "note_map=%u,%s\n", (unsigned)data->mappings[i].note, data->mappings[i].sfxPath);
    }

    fprintf(fd, "# lighting config (velocities 0-127; channel 0-15)\n");
    fprintf(fd, "light_channel=%u\n", (unsigned)data->globalLight.channel);
    fprintf(fd, "light_mapped_vel=%u\n", (unsigned)data->globalLight.mappedVel);
    fprintf(fd, "light_playing_vel=%u\n", (unsigned)data->globalLight.playingVel);
    fprintf(fd, "light_stop_all_vel=%u\n", (unsigned)data->globalLight.stopAllVel);

    uint32_t slCount = data->soundLightCount;
    if (slCount > MIDI_SOUND_LIGHTS_MAX) {
        slCount = MIDI_SOUND_LIGHTS_MAX;
    }
    if (slCount > 0) {
        fprintf(fd, "# per-sound lighting overrides: sfx,mapped_vel,playing_vel\n");
        for (uint32_t i = 0; i < slCount; ++i) {
            if (!data->soundLights[i].sfxPath[0]) {
                continue;
            }
            fprintf(fd,
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
        fprintf(fd, "# per-sound playback mode: sfx,mode(play|hold)\n");
        for (uint32_t i = 0; i < smCount; ++i) {
            if (!data->soundModes[i].sfxPath[0]) {
                continue;
            }
            fprintf(fd,
                    "sound_mode=%s,%s\n",
                    data->soundModes[i].sfxPath,
                    soundboardModeToString(data->soundModes[i].mode));
        }
    }

    for (uint32_t i = 0; i < data->soundOrderCount && i < SOUNDBOARD_ORDER_MAX; ++i) {
        if (fprintf(fd, "sound_order=%s\n", data->soundOrder[i].sfxPath) < 0) {
            printf("[CONFIG] [WARN] failed to write soundboard order: %s\n", strerror(errno));
            return RET_ERR;
        }
    }
    if (fprintf(fd, "soundboard_grid_initialized=%u\nsoundboard_layout=%s,%u,%u\n",
                (unsigned)(data->gridInitialized || data->soundboardLayout == SOUNDBOARD_LAYOUT_GRID ||
                           data->gridCellCount > 0),
                data->soundboardLayout == SOUNDBOARD_LAYOUT_GRID ? "grid" : "normal",
                (unsigned)(data->gridRows ? data->gridRows : 4),
                (unsigned)(data->gridColumns ? data->gridColumns : 4)) < 0) {
        printf("[CONFIG] [WARN] failed to write soundboard layout: %s\n", strerror(errno));
        return RET_ERR;
    }
    for (uint8_t i = 0; i < SOUNDBOARD_GRID_CORNER_COUNT; ++i) {
        if ((data->gridCornerMask & (uint8_t)(1U << i)) == 0) continue;
        static const char *const names[SOUNDBOARD_GRID_CORNER_COUNT] = {
            "top_left", "top_right", "bottom_left", "bottom_right",
        };
        if (fprintf(fd, "soundboard_grid_corner=%s,%u\n", names[i],
                    (unsigned)data->gridCornerNotes[i]) < 0) {
            printf("[CONFIG] [WARN] failed to write soundboard grid calibration: %s\n", strerror(errno));
            return RET_ERR;
        }
    }
    for (uint32_t i = 0; i < data->gridCellCount && i < SOUNDBOARD_GRID_CELLS_MAX; ++i) {
        if (!data->gridCells[i].sfxPath[0]) continue;
        if (fprintf(fd, "soundboard_grid_cell=%s,%u,%u\n", data->gridCells[i].sfxPath,
                    (unsigned)data->gridCells[i].row, (unsigned)data->gridCells[i].column) < 0) {
            printf("[CONFIG] [WARN] failed to write soundboard grid cell: %s\n", strerror(errno));
            return RET_ERR;
        }
    }

    uint32_t actionCount = data->actionMappingCount;
    if (actionCount > MIDI_ACTION_MAPPINGS_MAX) {
        actionCount = MIDI_ACTION_MAPPINGS_MAX;
    }
    if (actionCount > 0) {
        fprintf(fd, "# action mappings: note,action(stop_all|sampler_toggle)\n");
        for (uint32_t i = 0; i < actionCount; ++i) {
            const char *action = midiActionToString(data->actionMappings[i].action);
            if (!action) {
                continue;
            }
            fprintf(fd,
                    "note_action=%u,%s\n",
                    (unsigned)data->actionMappings[i].note,
                    action);
        }
    }

    fprintf(fd, "# sampler keyboard config\n");
    fprintf(fd, "sampler_keyboard_enabled=%u\n", (unsigned)data->samplerKeyboardEnabled);
    fprintf(fd, "sampler_keyboard_channel=%u\n", (unsigned)data->samplerKeyboardChannel);
    fprintf(fd, "sampler_root_note=%u\n", (unsigned)data->samplerRootNote);

    uint32_t cvCount = data->ccVolumeMappingCount;
    if (cvCount > MIDI_CC_VOLUME_MAPPINGS_MAX) {
        cvCount = MIDI_CC_VOLUME_MAPPINGS_MAX;
    }
    if (cvCount > 0) {
        fprintf(fd, "# MIDI CC to volume mappings: cc,thing_id\n");
        for (uint32_t i = 0; i < cvCount; ++i) {
            if (!data->ccVolumeMappings[i].thingId[0]) {
                continue;
            }
            fprintf(fd,
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
        fprintf(fd, "# effect state: effect_id,type,enabled,gain,drive,clip,output\n");
        for (uint32_t i = 0; i < fxStateCount; ++i) {
            if (!data->effectStates[i].effectId[0]) {
                continue;
            }
            fprintf(fd,
                    "fx_state=%s,%s,%u,%.6f,%.6f,%.6f,%.6f\n",
                    data->effectStates[i].effectId,
                    data->effectStates[i].type[0] ? data->effectStates[i].type : "gain",
                    (unsigned)(data->effectStates[i].enabled ? 1U : 0U),
                    (double)data->effectStates[i].gain,
                    (double)data->effectStates[i].drive,
                    (double)data->effectStates[i].clip,
                    (double)data->effectStates[i].output);
            if (data->effectStates[i].name[0]) {
                fprintf(fd, "fx_name=%s,%s\n", data->effectStates[i].effectId,
                        scriptHexEncode(data->effectStates[i].name).c_str());
            }
        }
    }

    // fx_param lines are no longer written here; they go to fx_params.txt instead.

    uint32_t fxCcCount = data->effectCcMappingCount;
    if (fxCcCount > MIDI_EFFECT_CC_MAPPINGS_MAX) {
        fxCcCount = MIDI_EFFECT_CC_MAPPINGS_MAX;
    }
    if (fxCcCount > 0) {
        fprintf(fd, "# effect MIDI CC mappings: cc,effect_id,param\n");
        for (uint32_t i = 0; i < fxCcCount; ++i) {
            if (!data->effectCcMappings[i].effectId[0] || !data->effectCcMappings[i].param[0]) {
                continue;
            }
            fprintf(fd,
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
        fprintf(fd, "# effect bypass toggle mappings: note,effect_id\n");
        for (uint32_t i = 0; i < fxToggleCount; ++i) {
            if (!data->effectToggleMappings[i].effectId[0]) {
                continue;
            }
            fprintf(fd,
                    "fx_toggle=%u,%s\n",
                    (unsigned)data->effectToggleMappings[i].note,
                    data->effectToggleMappings[i].effectId);
        }
    }

    uint32_t fxLightCount = data->effectLightMappingCount;
    if (fxLightCount > MIDI_EFFECT_LIGHT_MAPPINGS_MAX) {
        fxLightCount = MIDI_EFFECT_LIGHT_MAPPINGS_MAX;
    }
    if (fxLightCount > 0) {
        fprintf(fd, "# effect light mappings: effect_id,enabled_vel,bypassed_vel\n");
        for (uint32_t i = 0; i < fxLightCount; ++i) {
            if (!data->effectLightMappings[i].effectId[0]) {
                continue;
            }
            fprintf(fd,
                    "fx_light=%s,%u,%u\n",
                    data->effectLightMappings[i].effectId,
                    (unsigned)data->effectLightMappings[i].enabledVel,
                    (unsigned)data->effectLightMappings[i].bypassedVel);
        }
    }

    const int closeResult = output.commit();
    if (closeResult != 0) {
        printf("[CONFIG] [WARN] failed to finalize %s: %s\n", path, strerror(errno));
        return RET_ERR;
    }
    return RET_OK;
}

static int writeFxParamsPath(const char *path, const MidiEffectParam *params, uint32_t count) {
    if (!path) {
        return RET_ERR;
    }
    ConfigTextSchemaStatus schema =
        checkConfigTextFileSchema(path, "# audiox fx params v", FX_PARAMS_SCHEMA_VERSION);
    if (schema == ConfigTextSchemaStatus::Unsupported ||
        schema == ConfigTextSchemaStatus::Invalid ||
        schema == ConfigTextSchemaStatus::IoError) {
        printf("[CONFIG] [WARN] refusing to overwrite unsupported effect parameter schema in %s\n",
               path);
        return RET_ERR;
    }
    audiox::AtomicConfigFile output(path);
    FILE *fd = output.get();
    if (!fd) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", path, strerror(errno));
        return RET_ERR;
    }
    if (fprintf(fd, "# audiox fx params v%u\n", FX_PARAMS_SCHEMA_VERSION) < 0) {
        printf("[CONFIG] [WARN] failed to write effect parameter schema in %s: %s\n",
               path, strerror(errno));
        return RET_ERR;
    }
    if (params) {
        uint32_t cap = count > MIDI_EFFECT_PARAMS_MAX ? MIDI_EFFECT_PARAMS_MAX : count;
        for (uint32_t i = 0; i < cap; ++i) {
            if (!params[i].effectId[0] || !params[i].param[0]) {
                continue;
            }
            fprintf(fd, "fx_param=%s,%s,%.6f\n",
                    params[i].effectId,
                    params[i].param,
                    (double)params[i].value);
        }
    }
    if (output.commit() != 0) {
        printf("[CONFIG] [WARN] failed to finalize %s: %s\n", path, strerror(errno));
        return RET_ERR;
    }
    return RET_OK;
}

static int readFxParamsPath(const char *path, MidiEffectParam *out, uint32_t *count_out, uint32_t cap) {
    if (!path || !out || !count_out || cap == 0) {
        return RET_ERR;
    }
    *count_out = 0;
    FILE *fp = fopen(path, "r");
    if (!fp) {
        return (errno == ENOENT) ? RET_WARN : RET_ERR;
    }
    ConfigTextSchemaStatus schema =
        checkConfigTextSchema(fp, "# audiox fx params v", FX_PARAMS_SCHEMA_VERSION);
    if (schema == ConfigTextSchemaStatus::Unsupported ||
        schema == ConfigTextSchemaStatus::Invalid ||
        schema == ConfigTextSchemaStatus::IoError) {
        printf("[CONFIG] [WARN] unsupported or invalid effect parameter schema in %s\n", path);
        fclose(fp);
        return RET_ERR;
    }
    char line[320];
    while (fgets(line, sizeof(line), fp) && *count_out < cap) {
        char *eq = strchr(line, '=');
        if (!eq) {
            continue;
        }
        *eq = '\0';
        char *key = trimLeft(line);
        char *value = eq + 1;
        trimRight(key);
        trimRight(value);
        value = trimLeft(value);
        if (strcmp(key, "fx_param") == 0) {
            if (parseEffectParamLine(value, out, count_out) != RET_OK) {
                printf("[CONFIG] [WARN] invalid fx_param in %s\n", path);
            }
        }
    }
    int err = ferror(fp);
    fclose(fp);
    return err ? RET_ERR : RET_OK;
}

} // namespace

bool midiEffectNameValid(const char *name) {
    if (!name || strnlen(name, 64) >= 64) return false;
    const auto *p = reinterpret_cast<const unsigned char *>(name);
    while (*p) {
        if (*p < 32 || *p == 127) return false;
        if (*p < 128) { ++p; continue; }
        unsigned value = 0, bytes = 0, minimum = 0;
        if (*p >= 0xc2 && *p <= 0xdf) { value = *p & 31; bytes = 1; minimum = 0x80; }
        else if (*p >= 0xe0 && *p <= 0xef) { value = *p & 15; bytes = 2; minimum = 0x800; }
        else if (*p >= 0xf0 && *p <= 0xf4) { value = *p & 7; bytes = 3; minimum = 0x10000; }
        else return false;
        ++p;
        for (unsigned i = 0; i < bytes; ++i, ++p) {
            if ((*p & 0xc0) != 0x80) return false;
            value = (value << 6) | (*p & 63);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff) ||
            (value >= 0x80 && value <= 0x9f)) return false;
    }
    return true;
}

void midiEffectDisplayName(const MidiMapData &map, const char *id, const char *type,
                           char *out, size_t capacity) {
    for (uint32_t i = 0; i < std::min<uint32_t>(map.effectStateCount, MIDI_EFFECT_STATES_MAX); ++i) {
        if (!strcmp(map.effectStates[i].effectId, id) && map.effectStates[i].name[0]) {
            copyBounded(out, capacity, map.effectStates[i].name);
            return;
        }
    }
    const uint8_t pluginType = audiox::plugin::findType(type);
    const auto* pluginMetadata = audiox::plugin::metadataFor(pluginType);
    const char *label = audiox::plugin::validTypeName(type)
                        ? (pluginMetadata ? pluginMetadata->name : type) :
                        !strcmp(type, "gate") ? "Noise Gate" :
                        !strcmp(type, "cut") ? "Mute" :
                        !strcmp(type, "distortion") ? "Distortion" :
                        !strcmp(type, "pitch") ? "Pitch Shift" :
                        !strcmp(type, "compressor") ? "Compressor" :
                        !strcmp(type, "limiter") ? "Limiter" :
                        !strcmp(type, "eq") ? "Parametric EQ" :
                        !strcmp(type, "multiband") ? "Multiband Compressor" :
                        !strcmp(type, "noise_reduction") ? "Noise Reduction" :
                        !strcmp(type, "reverb") ? "Reverb" : "Gain";
    snprintf(out, capacity, "%s %s", label, !strncmp(id, "fx_", 3) ? id + 3 : id);
}

MidiMapData ConfigStore::readMidiMapFile() const {
    // MidiMapData is ~364KB: every path must return `data` so NRVO avoids a second stack copy.
    MidiMapData data = {};
    ConfigFileStamp mapBefore{}, fxBefore{}, scriptsBefore{};
    bool stamped = configFileStamp(MIDI_MAP_REAL_FILE_PATH, &mapBefore) &&
                   configFileStamp(FX_PARAMS_FILE_PATH, &fxBefore) &&
                   configFileStamp(SCRIPTS_FILE_PATH, &scriptsBefore);
    if (stamped) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (midiMapCached && midiMapCacheData &&
            configFileStampEqual(mapBefore, midiMapCacheStamp) &&
            configFileStampEqual(fxBefore, fxParamsCacheStamp) &&
            configFileStampEqual(scriptsBefore, scriptsCacheStamp)) {
            data = *midiMapCacheData;
            return data;
        }
    }
    timespec now{};
    clock_gettime(CLOCK_REALTIME, &now);
    int rc = readMidiMapPath(MIDI_MAP_REAL_FILE_PATH, &data);
    if (rc == RET_WARN) {
        // File doesn't exist yet; migrate from config.txt.
        printf("[CONFIG] midi_map.txt not found, migrating from config.txt\n");
        migrateMidiMapFromConfig(CONFIG_REAL_FILE_PATH, &data);
        (void)writeMidiMapPath(MIDI_MAP_REAL_FILE_PATH, &data);
    }
    if (rc == RET_ERR) return data;
    {
        MidiMapData scripts{};
        int scriptsRc = readMidiMapPath(SCRIPTS_FILE_PATH, &scripts, true);
        if (scriptsRc == RET_ERR) {
            printf("[CONFIG] [WARN] cannot read standalone scripts\n");
            return data;
        }
        if (scriptsRc == RET_OK) {
            data.chordCount = scripts.chordCount;
            memcpy(data.chords, scripts.chords, sizeof(data.chords));
        }
    }

    // fx_params.txt is the authoritative source for fx_param entries.
    // If it exists, it replaces any fx_param lines read from midi_map.txt.
    MidiEffectParam fxParams[MIDI_EFFECT_PARAMS_MAX] = {};
    uint32_t fxParamCount = 0;
    int fxRc = readFxParamsPath(FX_PARAMS_FILE_PATH, fxParams, &fxParamCount, MIDI_EFFECT_PARAMS_MAX);
    if (fxRc == RET_OK) {
        data.effectParamCount = fxParamCount;
        memcpy(data.effectParams, fxParams, fxParamCount * sizeof(MidiEffectParam));
    }
    // If fx_params.txt doesn't exist (RET_WARN), keep whatever was in midi_map.txt.

    ConfigFileStamp mapAfter{}, fxAfter{}, scriptsAfter{};
    if (rc == RET_OK && stamped &&
        configFileStamp(MIDI_MAP_REAL_FILE_PATH, &mapAfter) &&
        configFileStamp(FX_PARAMS_FILE_PATH, &fxAfter) &&
        configFileStamp(SCRIPTS_FILE_PATH, &scriptsAfter) &&
        configFileStampEqual(mapBefore, mapAfter) && configFileStampEqual(fxBefore, fxAfter) &&
        configFileStampEqual(scriptsBefore, scriptsAfter) &&
        configFileStampSettled(mapBefore, now) && configFileStampSettled(fxBefore, now) &&
        configFileStampSettled(scriptsBefore, now)) {
        std::lock_guard<std::mutex> lock(cacheMutex);
        if (!midiMapCacheData) midiMapCacheData.reset(new MidiMapData);
        *midiMapCacheData = data;
        midiMapCacheStamp = mapBefore;
        fxParamsCacheStamp = fxBefore;
        scriptsCacheStamp = scriptsBefore;
        midiMapCached = true;
    }
    return data;
}

int ConfigStore::writeMidiMapFile(MidiMapData *data) {
    if (!data) {
        return RET_ERR;
    }
    ConfigTextSchemaStatus midiSchema =
        checkConfigTextFileSchema(MIDI_MAP_REAL_FILE_PATH, "# audiox midi map v",
                                  MIDI_MAP_SCHEMA_VERSION);
    ConfigTextSchemaStatus paramsSchema =
        checkConfigTextFileSchema(FX_PARAMS_FILE_PATH, "# audiox fx params v",
                                  FX_PARAMS_SCHEMA_VERSION);
    ConfigTextSchemaStatus scriptsSchema =
        checkConfigTextFileSchema(SCRIPTS_FILE_PATH, "# audiox scripts v", SCRIPTS_SCHEMA_VERSION);
    auto schemaUnavailable = [](ConfigTextSchemaStatus status) {
        return status == ConfigTextSchemaStatus::Unsupported ||
               status == ConfigTextSchemaStatus::Invalid ||
               status == ConfigTextSchemaStatus::IoError;
    };
    if (schemaUnavailable(midiSchema) || schemaUnavailable(paramsSchema) || schemaUnavailable(scriptsSchema)) {
        printf("[CONFIG] [WARN] refusing to overwrite unsupported MIDI/effect configuration schema\n");
        return RET_ERR;
    }
    for (uint32_t i = 0; i < std::min<uint32_t>(data->effectStateCount, MIDI_EFFECT_STATES_MAX); ++i) {
        const auto &state = data->effectStates[i];
        if (strnlen(state.name, sizeof(state.name)) == sizeof(state.name) ||
            !midiEffectNameValid(state.name) ||
            !std::isfinite(state.gain) || !std::isfinite(state.drive) ||
            !std::isfinite(state.clip) || !std::isfinite(state.output)) {
            printf("[CONFIG] [WARN] rejected invalid effect name or non-finite effect state\n");
            return RET_ERR;
        }
    }
    for (uint32_t i = 0; i < std::min<uint32_t>(data->effectParamCount, MIDI_EFFECT_PARAMS_MAX); ++i) {
        if (!std::isfinite(data->effectParams[i].value)) {
            printf("[CONFIG] [WARN] rejected non-finite effect parameter\n");
            return RET_ERR;
        }
    }
    if (data->chordCount > SCRIPTS_MAX) {
        printf("[CONFIG] [WARN] Too many key chord mappings\n");
        return RET_ERR;
    }
    assignMacroIdentities(*data);
    for (uint32_t i = 0; i < data->chordCount; ++i) {
        uint8_t report[8];
        auto &chord = data->chords[i];
        char error[192]{};
        if (!scriptMigrate(chord, error, sizeof(error))) {
            printf("[CONFIG] [WARN] Cannot convert script %u: %s\n", chord.id, error);
            return RET_ERR;
        }
        if (chord.channel > 15 || chord.note > 127 ||
            (!chord.hookMode && chord.eventMask > SCRIPT_LEGACY_EVENTS_MASK) ||
            (chord.hookMode && !chord.script[0]) ||
            chord.idleVelocity > 127 || chord.pressedVelocity > 127 ||
            strnlen(chord.keys, sizeof(chord.keys)) == sizeof(chord.keys) ||
            strnlen(chord.script, sizeof(chord.script)) == sizeof(chord.script) ||
            strnlen(chord.name, sizeof(chord.name)) == sizeof(chord.name) ||
            (chord.keys[0] && !scriptChordReport(chord.keys, report)) ||
            (!chord.script[0] && !scriptChordReport(chord.keys, report)) ||
            (chord.script[0] && !scriptValidate(chord.script, error, sizeof(error)))) {
            printf("[CONFIG] [WARN] Invalid macro mapping: %s\n", error);
            return RET_ERR;
        }
        if (chord.hookMode && !scriptInspectHooks(chord.script, chord.eventMask, error, sizeof(error))) {
            printf("[CONFIG] [WARN] Invalid callback mapping: %s\n", error);
            return RET_ERR;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (chord.id == data->chords[j].id ||
                scriptMacroTriggersOverlap(chord, data->chords[j])) {
                printf("[CONFIG] [WARN] Duplicate macro trigger\n");
                return RET_ERR;
            }
        }
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
    uint32_t fxLightCount = data->effectLightMappingCount;
    if (fxLightCount > MIDI_EFFECT_LIGHT_MAPPINGS_MAX) {
        data->effectLightMappingCount = MIDI_EFFECT_LIGHT_MAPPINGS_MAX;
    }
    if (writeMidiMapPath(SCRIPTS_FILE_PATH, data, true) != RET_OK) return RET_ERR;
    uint32_t paramCount = data->effectParamCount;
    if (paramCount > MIDI_EFFECT_PARAMS_MAX) {
        paramCount = MIDI_EFFECT_PARAMS_MAX;
    }
    int fxRc = writeFxParamsPath(FX_PARAMS_FILE_PATH, data->effectParams, paramCount);
    if (fxRc != RET_OK) {
        invalidateConfigCache();
        return RET_ERR;
    }
    // Persist extracted parameters before removing their legacy MIDI representation.
    int rc = writeMidiMapPath(MIDI_MAP_REAL_FILE_PATH, data);
    invalidateConfigCache();
    if (rc != RET_OK || fxRc != RET_OK) {
        return RET_ERR;
    }
    return RET_OK;
}

int ConfigStore::purgeMissingSoundMappings(MidiMapData *data) {
    if (!data) {
        return RET_ERR;
    }

    MidiMapData cleaned = *data;
    bool changed = false;
    auto purge = [&](auto &entries, uint32_t &count, bool gridElements = false) -> int {
        if (count > sizeof(entries) / sizeof(entries[0])) {
            printf("[CONFIG] [WARN] invalid sound mapping count: %u\n", (unsigned)count);
            return RET_ERR;
        }
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; ++i) {
            char path[512];
            const char *sfx = entries[i].sfxPath;
            if (gridElements && soundboardGridAction(sfx) != MIDI_ACTION_NONE) {
                entries[kept++] = entries[i];
                continue;
            }
            int n = sfx[0] == '/' ? snprintf(path, sizeof(path), "%s", sfx)
                                  : snprintf(path, sizeof(path), "%s/%s", SFX_ROOT_DIR, sfx);
            if (n <= 0 || (size_t)n >= sizeof(path)) {
                printf("[CONFIG] [WARN] SFX path too long: %s\n", sfx);
                return RET_ERR;
            }
            struct stat st;
            if (stat(path, &st) < 0) {
                if (errno != ENOENT && errno != ENOTDIR) {
                    printf("[CONFIG] [WARN] failed to stat %s: %s\n", path, strerror(errno));
                    return RET_ERR;
                }
                changed = true;
                continue;
            }
            if (!S_ISREG(st.st_mode)) {
                changed = true;
                continue;
            }
            entries[kept++] = entries[i];
        }
        count = kept;
        return RET_OK;
    };

    if (purge(cleaned.mappings, cleaned.mappingCount) != RET_OK ||
        purge(cleaned.soundLights, cleaned.soundLightCount) != RET_OK ||
        purge(cleaned.soundModes, cleaned.soundModeCount) != RET_OK ||
        purge(cleaned.soundOrder, cleaned.soundOrderCount) != RET_OK ||
        purge(cleaned.gridCells, cleaned.gridCellCount, true) != RET_OK) {
        return RET_ERR;
    }
    if (changed && writeMidiMapFile(&cleaned) != RET_OK) {
        return RET_ERR;
    }
    *data = cleaned;
    return RET_OK;
}

int ConfigStore::listSoundboardFiles(char (*files)[MIDI_SFX_PATH_MAX], uint32_t capacity, uint32_t *count) const {
    if (!files || !count || !capacity) return RET_ERR;
    *count = 0;
    DIR *dir = opendir(SFX_ROOT_DIR);
    if (!dir) {
        if (errno == ENOENT) return RET_OK;
        printf("[CONFIG] [WARN] failed to open soundboard directory: %s\n", strerror(errno));
        return RET_ERR;
    }
    std::vector<std::string> names;
    int result = RET_OK;
    for (;;) {
        errno = 0;
        const dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) result = RET_ERR;
            break;
        }
        const size_t len = strlen(entry->d_name);
        if (len < 5 || len >= MIDI_SFX_PATH_MAX || strcasecmp(entry->d_name + len - 4, ".wav") != 0) continue;
        struct stat st;
        if (fstatat(dirfd(dir), entry->d_name, &st, 0) != 0) {
            if (errno == ENOENT) continue;
            result = RET_ERR;
            break;
        }
        if (S_ISREG(st.st_mode)) names.emplace_back(entry->d_name);
    }
    if (result != RET_OK) printf("[CONFIG] [WARN] failed to list soundboard files: %s\n", strerror(errno));
    if (closedir(dir) != 0) {
        printf("[CONFIG] [WARN] failed to close soundboard directory: %s\n", strerror(errno));
        result = RET_ERR;
    }
    if (result != RET_OK) return result;
    std::sort(names.begin(), names.end());
    const MidiMapData data = readMidiMapFile();
    auto append = [&](const std::string &name) {
        if (*count < capacity) copyBounded(files[(*count)++], MIDI_SFX_PATH_MAX, name.c_str());
    };
    for (uint32_t i = 0; i < data.soundOrderCount && i < SOUNDBOARD_ORDER_MAX; ++i) {
        const auto found = std::find(names.begin(), names.end(), data.soundOrder[i].sfxPath);
        if (found != names.end()) {
            append(*found);
            names.erase(found);
        }
    }
    for (const auto &name : names) append(name);
    return RET_OK;
}
