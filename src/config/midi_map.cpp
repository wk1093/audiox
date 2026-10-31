#include "config/context.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
        strcmp(parts[1], "cut") != 0) {
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

static int readMidiMapPath(const char *path, MidiMapData *out) {
    if (!path || !out) {
        return RET_ERR;
    }

    MidiMapData data = {};
    data.samplerKeyboardEnabled = 0;
    bool explicitSamplerEnabled = false;
    data.samplerKeyboardChannel = 0;
    data.samplerRootNote = 60;
    // defaults: channel 0, all states off (vel=0)

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return (errno == ENOENT) ? RET_WARN : RET_ERR;
    }

    char line[MIDI_SCRIPT_MAX * 2 + 384];
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
        } else if (strcmp(key, "note_action") == 0) {
            (void)parseMidiActionLine(value, data.actionMappings, &data.actionMappingCount);
        } else if (strcmp(key, "automation") == 0) {
            unsigned id, enabled, channel, note, idle, pressed;
            char nameHex[128]{}, keys[64]{};
            int offset = 0;
            MidiChord chord{};
            if (data.chordCount < MIDI_CHORDS_MAX &&
                sscanf(value, "%u,%u,%u,%u,%u,%u,%127[^,],%63[^,],%n",
                       &id, &enabled, &channel, &note, &idle, &pressed, nameHex, keys, &offset) == 8 &&
                offset > 0 && id > 0 && enabled <= 1 && channel <= 15 && note <= 127 &&
                idle <= 127 && pressed <= 127 &&
                midiHexDecode(nameHex, chord.name, sizeof(chord.name)) && chord.name[0] &&
                midiHexDecode(value + offset, chord.script, sizeof(chord.script))) {
                chord.id = id;
                chord.midiEnabled = enabled != 0;
                chord.channel = channel;
                chord.note = note;
                chord.idleVelocity = idle;
                chord.pressedVelocity = pressed;
                if (strcmp(keys, "-")) strcpy(chord.keys, keys);
                uint8_t report[8];
                bool valid = (chord.script[0] || chord.keys[0]) &&
                    (!chord.keys[0] || midiChordReport(chord.keys, report));
                for (uint32_t j = 0; j < data.chordCount; ++j)
                    if (data.chords[j].id == chord.id) valid = false;
                if (valid) {
                    data.chords[data.chordCount++] = chord;
                    continue;
                }
            }
            printf("[CONFIG] [WARN] Invalid or excess automation entry\n");
        } else if (strcmp(key, "key_macro") == 0) {
            unsigned channel, note, idle, pressed;
            char keys[64]{};
            int offset = 0;
            if (data.chordCount < MIDI_CHORDS_MAX &&
                sscanf(value, "%u,%u,%u,%u,%63[^,],%n", &channel, &note, &idle, &pressed, keys, &offset) == 5 &&
                offset > 0 && channel <= 15 && note <= 127 && idle <= 127 && pressed <= 127 &&
                strlen(value + offset) < MIDI_SCRIPT_MAX * 2) {
                MidiChord chord{};
                chord.channel = channel;
                chord.note = note;
                chord.idleVelocity = idle;
                chord.pressedVelocity = pressed;
                if (strcmp(keys, "-")) strcpy(chord.keys, keys);
                uint8_t report[8];
                if (midiHexDecode(value + offset, chord.script, sizeof(chord.script)) &&
                    (!chord.keys[0] || midiChordReport(chord.keys, report)) &&
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
            if (data.chordCount < MIDI_CHORDS_MAX &&
                sscanf(value, "%u,%u,%63[^,\r\n]%c", &channel, &note, keys, &extra) == 3 &&
                channel <= 15 && note <= 127 && midiChordReport(keys, report)) {
                MidiChord &chord = data.chords[data.chordCount++];
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
    for (uint32_t i = 0; i < data->chordCount && i < MIDI_CHORDS_MAX; ++i) {
        const auto &chord = data->chords[i];
        std::string hex = midiHexEncode(chord.script);
        if (dprintf(fd, "automation=%u,%u,%u,%u,%u,%u,%s,%s,%s\n",
                    chord.id, chord.midiEnabled ? 1 : 0, chord.channel, chord.note,
                    chord.idleVelocity, chord.pressedVelocity, midiHexEncode(chord.name).c_str(),
                    chord.keys[0] ? chord.keys : "-", hex.c_str()) < 0) {
            printf("[CONFIG] [WARN] Cannot write macro mapping\n");
            close(fd);
            return RET_ERR;
        }
    }
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

    for (uint32_t i = 0; i < data->soundOrderCount && i < SOUNDBOARD_ORDER_MAX; ++i) {
        if (dprintf(fd, "sound_order=%s\n", data->soundOrder[i].sfxPath) < 0) {
            printf("[CONFIG] [WARN] failed to write soundboard order: %s\n", strerror(errno));
            close(fd);
            return RET_ERR;
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

    // fx_param lines are no longer written here; they go to fx_params.txt instead.

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

    uint32_t fxLightCount = data->effectLightMappingCount;
    if (fxLightCount > MIDI_EFFECT_LIGHT_MAPPINGS_MAX) {
        fxLightCount = MIDI_EFFECT_LIGHT_MAPPINGS_MAX;
    }
    if (fxLightCount > 0) {
        dprintf(fd, "# effect light mappings: effect_id,enabled_vel,bypassed_vel\n");
        for (uint32_t i = 0; i < fxLightCount; ++i) {
            if (!data->effectLightMappings[i].effectId[0]) {
                continue;
            }
            dprintf(fd,
                    "fx_light=%s,%u,%u\n",
                    data->effectLightMappings[i].effectId,
                    (unsigned)data->effectLightMappings[i].enabledVel,
                    (unsigned)data->effectLightMappings[i].bypassedVel);
        }
    }

    const int closeResult = close(fd);
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
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", path, strerror(errno));
        return RET_ERR;
    }
    dprintf(fd, "# audiox fx params v1\n");
    if (params) {
        uint32_t cap = count > MIDI_EFFECT_PARAMS_MAX ? MIDI_EFFECT_PARAMS_MAX : count;
        for (uint32_t i = 0; i < cap; ++i) {
            if (!params[i].effectId[0] || !params[i].param[0]) {
                continue;
            }
            dprintf(fd, "fx_param=%s,%s,%.6f\n",
                    params[i].effectId,
                    params[i].param,
                    (double)params[i].value);
        }
    }
    if (close(fd) != 0) {
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

MidiMapData ConfigStore::readMidiMapFile() const {
    MidiMapData data = {};
    int rc = readMidiMapPath(MIDI_MAP_REAL_FILE_PATH, &data);
    if (rc == RET_WARN) {
        // File doesn't exist yet; migrate from config.txt.
        printf("[CONFIG] midi_map.txt not found, migrating from config.txt\n");
        migrateMidiMapFromConfig(CONFIG_REAL_FILE_PATH, &data);
        (void)writeMidiMapPath(MIDI_MAP_REAL_FILE_PATH, &data);
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

    return data;
}

int ConfigStore::writeMidiMapFile(MidiMapData *data) {
    if (!data) {
        return RET_ERR;
    }
    for (uint32_t i = 0; i < std::min<uint32_t>(data->effectStateCount, MIDI_EFFECT_STATES_MAX); ++i) {
        const auto &state = data->effectStates[i];
        if (!std::isfinite(state.gain) || !std::isfinite(state.drive) ||
            !std::isfinite(state.clip) || !std::isfinite(state.output)) {
            printf("[CONFIG] [WARN] rejected non-finite effect state\n");
            return RET_ERR;
        }
    }
    for (uint32_t i = 0; i < std::min<uint32_t>(data->effectParamCount, MIDI_EFFECT_PARAMS_MAX); ++i) {
        if (!std::isfinite(data->effectParams[i].value)) {
            printf("[CONFIG] [WARN] rejected non-finite effect parameter\n");
            return RET_ERR;
        }
    }
    if (data->chordCount > MIDI_CHORDS_MAX) {
        printf("[CONFIG] [WARN] Too many key chord mappings\n");
        return RET_ERR;
    }
    assignMacroIdentities(*data);
    for (uint32_t i = 0; i < data->chordCount; ++i) {
        uint8_t report[8];
        auto &chord = data->chords[i];
        char error[192]{};
        if (chord.channel > 15 || chord.note > 127 ||
            chord.idleVelocity > 127 || chord.pressedVelocity > 127 ||
            strnlen(chord.keys, sizeof(chord.keys)) == sizeof(chord.keys) ||
            strnlen(chord.script, sizeof(chord.script)) == sizeof(chord.script) ||
            strnlen(chord.name, sizeof(chord.name)) == sizeof(chord.name) ||
            (chord.keys[0] && !midiChordReport(chord.keys, report)) ||
            (!chord.script[0] && !midiChordReport(chord.keys, report)) ||
            (chord.script[0] && !midiValidateScript(chord.script, error, sizeof(error)))) {
            printf("[CONFIG] [WARN] Invalid macro mapping: %s\n", error);
            return RET_ERR;
        }
        for (uint32_t j = 0; j < i; ++j) {
            if (chord.id == data->chords[j].id ||
                (chord.midiEnabled && data->chords[j].midiEnabled &&
                 chord.channel == data->chords[j].channel && chord.note == data->chords[j].note)) {
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
    int rc = writeMidiMapPath(MIDI_MAP_REAL_FILE_PATH, data);
    uint32_t paramCount = data->effectParamCount;
    if (paramCount > MIDI_EFFECT_PARAMS_MAX) {
        paramCount = MIDI_EFFECT_PARAMS_MAX;
    }
    int fxRc = writeFxParamsPath(FX_PARAMS_FILE_PATH, data->effectParams, paramCount);
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
    auto purge = [&](auto &entries, uint32_t &count) -> int {
        if (count > sizeof(entries) / sizeof(entries[0])) {
            printf("[CONFIG] [WARN] invalid sound mapping count: %u\n", (unsigned)count);
            return RET_ERR;
        }
        uint32_t kept = 0;
        for (uint32_t i = 0; i < count; ++i) {
            char path[512];
            const char *sfx = entries[i].sfxPath;
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
        purge(cleaned.soundOrder, cleaned.soundOrderCount) != RET_OK) {
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
