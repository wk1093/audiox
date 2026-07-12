#include "config/context.hpp"

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>


ConfigData ConfigStore::readConfigFile() const {
    ConfigData cfg = {};
    FILE *fp = fopen(CONFIG_REAL_FILE_PATH, "r");
    if (!fp) {
        if (errno != ENOENT) {
            printf("[CONFIG] [WARN] failed to open %s for reading: %s\n", CONFIG_REAL_FILE_PATH, strerror(errno));
        }
        return cfg;
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

        char *newline = strchr(value, '\n');
        if (newline) {
            *newline = '\0';
        }

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
        printf("[CONFIG] [WARN] failed while reading %s: %s\n", CONFIG_REAL_FILE_PATH, strerror(errno));
    }
    fclose(fp);
    return cfg;
}

int ConfigStore::writeConfigFile(ConfigData *cfg) {
    int fd = open(CONFIG_REAL_FILE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", CONFIG_REAL_FILE_PATH, strerror(errno));
        return -1;
    }
    dprintf(fd, "usb_sample_rate=%d\n", cfg->sampleRate);
    dprintf(fd, "usb_playback_channels=%d\n", cfg->playbackChannels);
    dprintf(fd, "usb_capture_channels=%d\n", cfg->captureChannels);
    dprintf(fd, "usb_sample_size=%d\n", cfg->sampleSize);
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
    if (cfg.sampleRate == 0) {
        cfg.sampleRate = SAMPLE_RATE;
    }
    if (cfg.playbackChannels == 0) {
        cfg.playbackChannels = 2;
    }
    if (cfg.captureChannels == 0) {
        cfg.captureChannels = 2;
    }
    if (cfg.sampleSize == 0) {
        cfg.sampleSize = 2;
    }

    writeConfigFile(&cfg);
}

RouterConfig ConfigStore::router() const {
    return RouterConfig();
}
