#include "config/context.hpp"

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

namespace {

static inline void normalizeConfig(ConfigData *cfg) {
    if (!cfg) {
        return;
    }

    if (cfg->sampleRate == 0) {
        cfg->sampleRate = SAMPLE_RATE;
    }
    if (cfg->playbackChannels == 0) {
        cfg->playbackChannels = 2;
    }
    if (cfg->captureChannels == 0) {
        cfg->captureChannels = 2;
    }
    if (cfg->sampleSize == 0) {
        cfg->sampleSize = 2;
    }
    if (cfg->mappingCount > MIDI_MAPPINGS_MAX) {
        cfg->mappingCount = MIDI_MAPPINGS_MAX;
    }
}

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

static int parseMidiMapLine(const char *value,
                            ConfigData::MidiMapping *out,
                            uint32_t *count_in_out) {
    if (!value || !out || !count_in_out) {
        return RET_ERR;
    }
    if (*count_in_out >= MIDI_MAPPINGS_MAX) {
        return RET_ERR;
    }

    char mapCopy[256];
    copyBounded(mapCopy, sizeof(mapCopy), value);

    char *comma = strchr(mapCopy, ',');
    if (!comma) {
        return RET_ERR;
    }

    *comma = '\0';
    char *noteStr = trimLeft(mapCopy);
    char *pathStr = trimLeft(comma + 1);
    trimRight(noteStr);
    trimRight(pathStr);

    char *endp = NULL;
    long note = strtol(noteStr, &endp, 10);
    if (!endp || *endp != '\0' || note < 0 || note > 127) {
        return RET_ERR;
    }
    if (!pathStr[0]) {
        return RET_ERR;
    }

    ConfigData::MidiMapping *slot = &out[*count_in_out];
    slot->note = (uint8_t)note;
    copyBounded(slot->sfxPath, sizeof(slot->sfxPath), pathStr);
    ++(*count_in_out);
    return RET_OK;
}

static int readConfigPath(const char *path, ConfigData *out, int warnIfMissing) {
    if (!path || !out) {
        return RET_ERR;
    }

    ConfigData cfg = {};
    FILE *fp = fopen(path, "r");
    if (!fp) {
        if (errno != ENOENT || warnIfMissing) {
            printf("[CONFIG] [WARN] failed to open %s for reading: %s\n", path, strerror(errno));
        }
        return (errno == ENOENT) ? RET_WARN : RET_ERR;
    }

    char line[256];
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

        if (strcmp(key, "usb_sample_rate") == 0) {
            cfg.sampleRate = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "usb_playback_channels") == 0) {
            cfg.playbackChannels = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "usb_capture_channels") == 0) {
            cfg.captureChannels = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "usb_sample_size") == 0) {
            cfg.sampleSize = (uint32_t)strtoul(value, NULL, 10);
        } else if (strcmp(key, "midi_map") == 0) {
            (void)parseMidiMapLine(value, cfg.mappings, &cfg.mappingCount);
        } else if (strncmp(key, "midi_note_", 10) == 0) {
            char mapLine[256];
            int n = snprintf(mapLine, sizeof(mapLine), "%s,%s", key + 10, value);
            if (n > 0 && (size_t)n < sizeof(mapLine)) {
                (void)parseMidiMapLine(mapLine, cfg.mappings, &cfg.mappingCount);
            }
        }
    }

    if (ferror(fp)) {
        printf("[CONFIG] [WARN] failed while reading %s: %s\n", path, strerror(errno));
        fclose(fp);
        return RET_ERR;
    }
    fclose(fp);

    normalizeConfig(&cfg);
    *out = cfg;
    return RET_OK;
}

} // namespace


ConfigData ConfigStore::readConfigFile() const {
    ConfigData cfg = {};
    (void)readConfigPath(CONFIG_REAL_FILE_PATH, &cfg, 0);
    return cfg;
}

int ConfigStore::readStagingConfigFile(ConfigData *out) const {
    return readConfigPath(CONFIG_STAGING_FILE_PATH, out, 0);
}

int ConfigStore::writeConfigFile(ConfigData *cfg) {
    if (!cfg) {
        return -1;
    }

    normalizeConfig(cfg);

    int fd = open(CONFIG_REAL_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", CONFIG_REAL_FILE_PATH, strerror(errno));
        return -1;
    }
    dprintf(fd, "usb_sample_rate=%u\n", cfg->sampleRate);
    dprintf(fd, "usb_playback_channels=%u\n", cfg->playbackChannels);
    dprintf(fd, "usb_capture_channels=%u\n", cfg->captureChannels);
    dprintf(fd, "usb_sample_size=%u\n", cfg->sampleSize);
    for (uint32_t i = 0; i < cfg->mappingCount && i < MIDI_MAPPINGS_MAX; ++i) {
        if (!cfg->mappings[i].sfxPath[0]) {
            continue;
        }
        dprintf(fd, "midi_map=%u,%s\n", (unsigned)cfg->mappings[i].note, cfg->mappings[i].sfxPath);
    }
    close(fd);
    return 0;
}

ConfigStore::ConfigStore(Audiox *context) : app(context) {
    if (!app) {
        printf("[CONFIG] [ERR] ConfigStore initialized with null context\n");
    } else {
        app->config = this;
    }

    ConfigData cfg = readConfigFile();
    writeConfigFile(&cfg);
}

int ConfigStore::findMidiMapping(uint8_t note, ConfigData::MidiMapping *out) const {
    ConfigData cfg = readConfigFile();
    uint32_t count = cfg.mappingCount;
    if (count > MIDI_MAPPINGS_MAX) {
        count = MIDI_MAPPINGS_MAX;
    }

    for (uint32_t i = 0; i < count; ++i) {
        if (cfg.mappings[i].note != note) {
            continue;
        }
        if (!cfg.mappings[i].sfxPath[0]) {
            continue;
        }

        if (out) {
            *out = cfg.mappings[i];
        }
        return RET_OK;
    }

    return RET_ERR;
}

RouterConfig ConfigStore::router() const {
    return RouterConfig();
}
