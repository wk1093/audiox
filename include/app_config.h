#ifndef APP_CONFIG_H
#define APP_CONFIG_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "init_defs.h"

typedef struct app_runtime_config {
    unsigned int usb_playback_channels;
    unsigned int usb_capture_channels;
    unsigned int usb_sample_rate;
    unsigned int usb_sample_size;
} app_runtime_config_t;

static inline void app_runtime_config_defaults(app_runtime_config_t *cfg) {
    if (!cfg) {
        return;
    }

    cfg->usb_playback_channels = 2;
    cfg->usb_capture_channels = 2;
    cfg->usb_sample_rate = SAMPLE_RATE;
    cfg->usb_sample_size = 2;
}

static inline unsigned int app_parse_u32_or_default(const char *value, unsigned int fallback) {
    if (!value) {
        return fallback;
    }

    while (*value == ' ' || *value == '\t') {
        ++value;
    }

    char *endp = NULL;
    unsigned long v = strtoul(value, &endp, 10);
    if (endp == value) {
        return fallback;
    }
    while (endp && (*endp == ' ' || *endp == '\t')) {
        ++endp;
    }
    if (endp && *endp != '\0' && *endp != '\n' && *endp != '\r') {
        return fallback;
    }
    return (unsigned int)v;
}

static inline int app_load_runtime_config_file(const char *path, app_runtime_config_t *cfg) {
    if (!path || !cfg) {
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        return -1;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r') {
            continue;
        }

        char *nl = p + strlen(p);
        while (nl > p && (nl[-1] == '\n' || nl[-1] == '\r' || nl[-1] == ' ' || nl[-1] == '\t')) {
            --nl;
        }
        *nl = '\0';

        char *eq = strchr(p, '=');
        if (!eq) {
            continue;
        }

        *eq = '\0';
        char *key = p;
        char *value = eq + 1;
        while (*value == ' ' || *value == '\t') {
            ++value;
        }

        if (strcmp(key, "usb_playback_channels") == 0) {
            cfg->usb_playback_channels = app_parse_u32_or_default(value, cfg->usb_playback_channels);
        } else if (strcmp(key, "usb_capture_channels") == 0) {
            cfg->usb_capture_channels = app_parse_u32_or_default(value, cfg->usb_capture_channels);
        } else if (strcmp(key, "usb_sample_rate") == 0) {
            cfg->usb_sample_rate = app_parse_u32_or_default(value, cfg->usb_sample_rate);
        } else if (strcmp(key, "usb_sample_size") == 0) {
            cfg->usb_sample_size = app_parse_u32_or_default(value, cfg->usb_sample_size);
        }
    }

    fclose(fp);
    return 0;
}

static inline int app_runtime_config_is_valid(const app_runtime_config_t *cfg) {
    if (!cfg) {
        return 0;
    }

    if (cfg->usb_playback_channels < 1 || cfg->usb_playback_channels > 16) {
        return 0;
    }
    if (cfg->usb_capture_channels < 1 || cfg->usb_capture_channels > 16) {
        return 0;
    }
    if (cfg->usb_sample_rate < 8000 || cfg->usb_sample_rate > 192000) {
        return 0;
    }
    if (cfg->usb_sample_size < 1 || cfg->usb_sample_size > 4) {
        return 0;
    }

    return 1;
}

#endif
