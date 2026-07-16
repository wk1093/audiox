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

RouterConfig ConfigStore::router() const {
    return RouterConfig();
}
