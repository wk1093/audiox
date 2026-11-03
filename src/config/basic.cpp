#include "config/context.hpp"

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <cmath>
#include <sys/stat.h>

namespace {

static int streqIgnoreCase(const char *a, const char *b) {
    if (!a || !b) {
        return 0;
    }
    while (*a && *b) {
        char ca = *a;
        char cb = *b;
        if (ca >= 'A' && ca <= 'Z') {
            ca = (char)(ca - 'A' + 'a');
        }
        if (cb >= 'A' && cb <= 'Z') {
            cb = (char)(cb - 'A' + 'a');
        }
        if (ca != cb) {
            return 0;
        }
        ++a;
        ++b;
    }
    return (*a == '\0' && *b == '\0') ? 1 : 0;
}

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
    if (cfg->soundboardMode != SOUNDBOARD_MODE_HOLD) {
        cfg->soundboardMode = SOUNDBOARD_MODE_PLAY;
    }
    if (cfg->midiPassthrough > 2) cfg->midiPassthrough = 0;
    cfg->keyboardMacros = cfg->keyboardMacros ? 1 : 0;
}

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
        } else if (strcmp(key, "soundboard_mode") == 0) {
            cfg.soundboardMode = soundboardModeFromString(value);
        } else if (strcmp(key, "framebuffer_view") == 0) {
            cfg.framebufferSoundboard = strcmp(value, "soundboard") == 0 ? 1 : 0;
        } else if (strcmp(key, "midi_passthrough") == 0) {
            cfg.midiPassthrough = strcmp(value, "unmapped") == 0 ? 1 : strcmp(value, "all") == 0 ? 2 : 0;
        } else if (strcmp(key, "keyboard_macros") == 0) {
            cfg.keyboardMacros = strcmp(value, "1") == 0;
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

namespace {
bool scriptConfigPath(const char *name, char *path, size_t size) {
    if (!name) return false;
    for (const char *allowed : SCRIPT_CONFIG_FILES) {
        if (strcmp(name, allowed) == 0) {
            int n = snprintf(path, size, "%s/%s", ROOT_MOUNT_POINT, name);
            return n > 0 && static_cast<size_t>(n) < size;
        }
    }
    return false;
}
}

int ConfigStore::readScriptConfig(const char *name, char *out, size_t capacity, size_t *length) const {
    char path[256]{};
    if (!out || !length || !capacity || !scriptConfigPath(name, path, sizeof(path))) {
        printf("[CONFIG] [WARN] Lua requested an invalid configuration file\n");
        return RET_ERR;
    }
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) ||
        st.st_size > static_cast<off_t>(MIDI_SCRIPT_CONFIG_READ_MAX)) {
        printf("[CONFIG] [WARN] Lua cannot read %s: %s\n", path,
               fd < 0 ? strerror(errno) : "not a readable regular file within 64 KiB");
        if (fd >= 0) close(fd);
        return RET_ERR;
    }
    size_t used = 0;
    bool ok = true;
    while (used < capacity) {
        ssize_t n = read(fd, out + used, capacity - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { ok = false; break; }
        if (!n) break;
        used += static_cast<size_t>(n);
    }
    if (used == capacity || memchr(out, '\0', used)) ok = false;
    if (close(fd) < 0) ok = false;
    if (!ok) {
        printf("[CONFIG] [WARN] Lua configuration read failed or exceeded the text limit: %s\n", path);
        return RET_ERR;
    }
    out[used] = '\0';
    *length = used;
    return RET_OK;
}

int ConfigStore::writeScriptConfig(const char *name, const char *text, size_t length) {
    char path[256]{};
    if (!midiScriptConfigTextValid(text, length) ||
        !scriptConfigPath(name, path, sizeof(path))) {
        printf("[CONFIG] [WARN] Invalid Lua configuration write\n");
        return RET_ERR;
    }
    char temporary[288]{};
    snprintf(temporary, sizeof(temporary), "%s.lua-XXXXXX", path);
    int fd = mkstemp(temporary);
    if (fd < 0) {
        printf("[CONFIG] [WARN] Cannot stage Lua configuration: %s\n", strerror(errno));
        return RET_ERR;
    }
    bool ok = fchmod(fd, 0644) == 0;
    size_t written = 0;
    while (ok && written < length) {
        ssize_t n = write(fd, text + written, length - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        written += static_cast<size_t>(n);
    }
    if (ok && fsync(fd) < 0) ok = false;
    if (close(fd) < 0) ok = false;
    if (ok && rename(temporary, path) < 0) ok = false;
    if (ok) {
        int directory = open(ROOT_MOUNT_POINT, O_RDONLY | O_DIRECTORY);
        if (directory < 0) ok = false;
        else {
            if (fsync(directory) < 0) ok = false;
            if (close(directory) < 0) ok = false;
        }
    }
    if (!ok) {
        printf("[CONFIG] [WARN] Lua configuration save failed (%s): %s\n", path, strerror(errno));
        if (unlink(temporary) < 0 && errno != ENOENT)
            printf("[CONFIG] [WARN] Lua staging cleanup failed: %s\n", strerror(errno));
        return RET_ERR;
    }
    printf("[CONFIG] [LUA] Saved %s; reload/restart may be required\n", name);
    return RET_OK;
}

ConfigData ConfigStore::readConfigFile() const {
    ConfigData cfg = {};
    (void)readConfigPath(CONFIG_REAL_FILE_PATH, &cfg, 0);
    return cfg;
}

int ConfigStore::readScriptSettings(ConfigData *out) const {
    int result = readConfigPath(CONFIG_REAL_FILE_PATH, out, 1);
    if (result != RET_OK)
        printf("[CONFIG] [WARN] Cannot read structured Lua settings\n");
    return result;
}

bool applyScriptSetting(ConfigData &cfg, const char *key, const MidiScriptStateEntry &value) {
    if (!key) return false;
    auto numeric = [&](uint32_t min, uint32_t max, uint32_t &target) {
        if (value.kind != MidiScriptValueKind::Number || !std::isfinite(value.numberValue) ||
            value.numberValue < min || value.numberValue > max ||
            std::floor(value.numberValue) != value.numberValue) return false;
        target = static_cast<uint32_t>(value.numberValue);
        return true;
    };
    auto boolean = [&](uint8_t &target) {
        if (value.kind != MidiScriptValueKind::Boolean) return false;
        target = value.booleanValue;
        return true;
    };
    auto choice = [&](const char *a, const char *b, const char *c, uint8_t &target) {
        if (value.kind != MidiScriptValueKind::String) return false;
        if (!strcmp(value.stringValue, a)) target = 0;
        else if (!strcmp(value.stringValue, b)) target = 1;
        else if (c && !strcmp(value.stringValue, c)) target = 2;
        else return false;
        return true;
    };
    if (!strcmp(key, "usb_sample_rate")) return numeric(8000, 192000, cfg.sampleRate);
    if (!strcmp(key, "usb_playback_channels")) return numeric(1, 16, cfg.playbackChannels);
    if (!strcmp(key, "usb_capture_channels")) return numeric(1, 16, cfg.captureChannels);
    if (!strcmp(key, "usb_sample_size")) return numeric(1, 4, cfg.sampleSize);
    if (!strcmp(key, "keyboard_macros")) return boolean(cfg.keyboardMacros);
    if (!strcmp(key, "soundboard_mode")) return choice("play", "hold", nullptr, cfg.soundboardMode);
    if (!strcmp(key, "framebuffer_view")) return choice("meters", "soundboard", nullptr, cfg.framebufferSoundboard);
    if (!strcmp(key, "midi_passthrough")) return choice("off", "unmapped", "all", cfg.midiPassthrough);
    return false;
}

int ConfigStore::writeScriptSetting(const char *key, const MidiScriptStateEntry &value) {
    ConfigData cfg{};
    if (readScriptSettings(&cfg) != RET_OK || !applyScriptSetting(cfg, key, value)) {
        printf("[CONFIG] [WARN] Invalid structured Lua setting\n");
        return RET_ERR;
    }
    if (writeConfigFile(&cfg) != RET_OK) return RET_ERR;
    const int directory = open(ROOT_MOUNT_POINT, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    bool ok = directory >= 0;
    int error = errno;
    if (ok && fsync(directory) < 0) { ok = false; error = errno; }
    if (directory >= 0 && close(directory) < 0 && ok) { ok = false; error = errno; }
    if (!ok) {
        printf("[CONFIG] [WARN] Cannot flush structured Lua settings: %s\n", strerror(error));
        return RET_ERR;
    }
    return RET_OK;
}

int ConfigStore::readStagingConfigFile(ConfigData *out) const {
    return readConfigPath(CONFIG_STAGING_FILE_PATH, out, 0);
}

int ConfigStore::writeConfigFile(ConfigData *cfg) {
    if (!cfg) {
        return -1;
    }

    normalizeConfig(cfg);

    const char *temporary = CONFIG_REAL_FILE_PATH ".tmp";
    int fd = open(temporary, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", CONFIG_REAL_FILE_PATH, strerror(errno));
        return -1;
    }
    int n = dprintf(fd, "usb_sample_rate=%u\nusb_playback_channels=%u\nusb_capture_channels=%u\n"
                   "usb_sample_size=%u\nsoundboard_mode=%s\nframebuffer_view=%s\n"
                   "midi_passthrough=%s\nkeyboard_macros=%u\n",
                   cfg->sampleRate, cfg->playbackChannels, cfg->captureChannels, cfg->sampleSize,
                   soundboardModeToString(cfg->soundboardMode), cfg->framebufferSoundboard ? "soundboard" : "meters",
                   cfg->midiPassthrough == 1 ? "unmapped" : cfg->midiPassthrough == 2 ? "all" : "off",
                   cfg->keyboardMacros);
    bool ok = n >= 0 && fsync(fd) == 0;
    int error = errno;
    if (close(fd) < 0 && ok) { ok = false; error = errno; }
    if (ok && rename(temporary, CONFIG_REAL_FILE_PATH) < 0) { ok = false; error = errno; }
    if (!ok) {
        printf("[CONFIG] [WARN] failed to save %s: %s\n", CONFIG_REAL_FILE_PATH, strerror(error));
        if (unlink(temporary) < 0 && errno != ENOENT) {
            printf("[CONFIG] [WARN] failed to remove %s: %s\n", temporary, strerror(errno));
        }
        return RET_ERR;
    }
    return 0;
}

const char *soundboardModeToString(uint8_t mode) {
    return (mode == SOUNDBOARD_MODE_HOLD) ? "hold" : "play";
}

uint8_t soundboardModeFromString(const char *value) {
    if (!value) {
        return SOUNDBOARD_MODE_PLAY;
    }
    if (streqIgnoreCase(value, "hold") || strcmp(value, "1") == 0) {
        return SOUNDBOARD_MODE_HOLD;
    }
    return SOUNDBOARD_MODE_PLAY;
}

uint8_t soundboardGridAction(const char *element) {
    if (!element) return MIDI_ACTION_NONE;
    if (strcmp(element, SOUNDBOARD_GRID_STOP_ALL_ID) == 0) return MIDI_ACTION_STOP_ALL;
    if (strcmp(element, SOUNDBOARD_GRID_SAMPLER_TOGGLE_ID) == 0) return MIDI_ACTION_SAMPLER_TOGGLE;
    return MIDI_ACTION_NONE;
}

bool soundboardGridNote(const MidiMapData *data, uint8_t row, uint8_t column, uint8_t *note) {
    if (!data || !note || data->gridRows < 2 || data->gridRows > SOUNDBOARD_GRID_ROWS_MAX ||
        data->gridColumns < 2 || data->gridColumns > SOUNDBOARD_GRID_COLUMNS_MAX ||
        row >= data->gridRows || column >= data->gridColumns ||
        (data->gridCornerMask & 0x0f) != 0x0f) {
        return false;
    }

    const int topLeft = data->gridCornerNotes[SOUNDBOARD_GRID_TOP_LEFT];
    const int topRight = data->gridCornerNotes[SOUNDBOARD_GRID_TOP_RIGHT];
    const int bottomLeft = data->gridCornerNotes[SOUNDBOARD_GRID_BOTTOM_LEFT];
    const int bottomRight = data->gridCornerNotes[SOUNDBOARD_GRID_BOTTOM_RIGHT];
    const int columnSteps = (int)data->gridColumns - 1;
    const int rowSteps = (int)data->gridRows - 1;
    const int horizontalDelta = topRight - topLeft;
    const int verticalDelta = bottomLeft - topLeft;
    if (horizontalDelta % columnSteps != 0 || verticalDelta % rowSteps != 0 ||
        topLeft + horizontalDelta + verticalDelta != bottomRight) {
        return false;
    }

    const int inferred = topLeft + (horizontalDelta / columnSteps) * column +
                         (verticalDelta / rowSteps) * row;
    if (inferred < 0 || inferred > 127) return false;
    *note = (uint8_t)inferred;
    return true;
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

RouterConfig ConfigStore::router() const {
    return RouterConfig();
}

int ConfigStore::readVolumesFile(VolumeEntry *out, uint32_t *count_out, uint32_t cap) const {
    if (!out || !count_out || cap == 0) {
        return RET_ERR;
    }
    *count_out = 0;

    FILE *fp = fopen(VOLUMES_FILE_PATH, "r");
    if (!fp) {
        return (errno == ENOENT) ? RET_WARN : RET_ERR;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) && *count_out < cap) {
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

        if (!key[0] || key[0] == '#') {
            continue;
        }
        if (strnlen(key, sizeof(out->thingId)) >= sizeof(out->thingId)) {
            continue;
        }

        char *endp = NULL;
        double g = strtod(value, &endp);
        if (!endp || endp == value || *endp != '\0' || !std::isfinite(g)) {
            printf("[CONFIG] [WARN] invalid volume for %s in %s\n", key, VOLUMES_FILE_PATH);
            fclose(fp);
            return RET_ERR;
        }
        if (g < 0.0) g = 0.0;
        if (g > 2.0) g = 2.0;

        copyBounded(out[*count_out].thingId, sizeof(out[*count_out].thingId), key);
        out[*count_out].gain = (float)g;
        ++(*count_out);
    }

    const bool failed = ferror(fp);
    const int closeRc = fclose(fp);
    if (failed || closeRc != 0) {
        printf("[CONFIG] [WARN] failed to read %s: %s\n", VOLUMES_FILE_PATH, strerror(errno));
        return RET_ERR;
    }
    return RET_OK;
}

int ConfigStore::writeVolumesFile(const VolumeEntry *entries, uint32_t count) {
    if (!entries && count > 0) {
        return RET_ERR;
    }
    if (count > VOLUME_ENTRIES_MAX) {
        printf("[CONFIG] [WARN] volume table exceeds capacity\n");
        return RET_ERR;
    }
    for (uint32_t i = 0; i < count; ++i) {
        if (!std::isfinite(entries[i].gain) || entries[i].gain < 0.0f || entries[i].gain > 2.0f) {
            printf("[CONFIG] [WARN] invalid gain while saving %s\n", VOLUMES_FILE_PATH);
            return RET_ERR;
        }
    }
    char temporary[] = VOLUMES_FILE_PATH ".tmp.XXXXXX";
    int fd = mkstemp(temporary);
    if (fd < 0) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", VOLUMES_FILE_PATH, strerror(errno));
        return RET_ERR;
    }

    bool ok = fchmod(fd, 0644) == 0 && dprintf(fd, "# audiox volumes v1\n") >= 0;
    for (uint32_t i = 0; ok && i < count; ++i) {
        if (!entries[i].thingId[0]) {
            continue;
        }
        ok = dprintf(fd, "%s=%.4f\n", entries[i].thingId, (double)entries[i].gain) >= 0;
    }
    if (ok) ok = fsync(fd) == 0;
    int error = errno;
    if (close(fd) < 0 && ok) { ok = false; error = errno; }
    if (ok && rename(temporary, VOLUMES_FILE_PATH) < 0) { ok = false; error = errno; }
    if (!ok) {
        printf("[CONFIG] [WARN] failed to save %s: %s\n", VOLUMES_FILE_PATH, strerror(error));
        if (unlink(temporary) < 0 && errno != ENOENT) {
            printf("[CONFIG] [WARN] failed to remove %s: %s\n", temporary, strerror(errno));
        }
        return RET_ERR;
    }
    int directory = open(ROOT_MOUNT_POINT, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) {
        printf("[CONFIG] [WARN] cannot open volume directory for sync: %s\n", strerror(errno));
        return RET_ERR;
    }
    ok = fsync(directory) == 0;
    error = errno;
    if (close(directory) < 0 && ok) { ok = false; error = errno; }
    if (!ok) {
        printf("[CONFIG] [WARN] failed to sync volume directory: %s\n", strerror(error));
        return RET_ERR;
    }
    return RET_OK;
}
