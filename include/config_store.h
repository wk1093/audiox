#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

typedef void (*config_store_log_fn_t)(void *ctx, const char *line);

typedef struct config_store {
    const char *mount_point;
    const char *device_path;
    const char *file_path;
    const char *staging_file_path;
    int mount_active;
} config_store_t;

static inline void config_store_logf(config_store_log_fn_t log_fn,
                                     void *log_ctx,
                                     const char *fmt,
                                     const char *a,
                                     const char *b,
                                     const char *c) {
    if (!log_fn) {
        return;
    }

    char line[192];
    snprintf(line, sizeof(line), fmt, a ? a : "", b ? b : "", c ? c : "");
    log_fn(log_ctx, line);
}

static inline int config_store_path_exists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0) ? 1 : 0;
}

static inline int config_store_is_mount_active(const char *mount_point) {
    if (!mount_point) {
        return 0;
    }

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return 0;
    }

    char line[512];
    int active = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char src[160];
        char dst[160];
        char fstype[64];
        if (sscanf(line, "%159s %159s %63s", src, dst, fstype) == 3) {
            if (strcmp(dst, mount_point) == 0) {
                active = 1;
                break;
            }
        }
    }

    fclose(fp);
    return active;
}

static inline void config_store_init(config_store_t *cfg,
                                     const char *mount_point,
                                     const char *device_path,
                                     const char *file_path,
                                     const char *staging_file_path) {
    if (!cfg) {
        return;
    }

    cfg->mount_point = mount_point;
    cfg->device_path = device_path;
    cfg->file_path = file_path;
    cfg->staging_file_path = staging_file_path;
    cfg->mount_active = config_store_is_mount_active(mount_point);
}

static inline int config_store_copy_file(const char *src_path, const char *dst_path) {
    if (!src_path || !dst_path) {
        return -1;
    }

    FILE *src = fopen(src_path, "r");
    if (!src) {
        return -1;
    }

    FILE *dst = fopen(dst_path, "w");
    if (!dst) {
        fclose(src);
        return -1;
    }

    char buf[1024];
    int ok = 1;
    size_t n = 0;
    while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
        if (fwrite(buf, 1, n, dst) != n) {
            ok = 0;
            break;
        }
    }

    if (ferror(src)) {
        ok = 0;
    }

    if (fflush(dst) != 0) {
        ok = 0;
    }
    if (fsync(fileno(dst)) != 0) {
        ok = 0;
    }

    fclose(dst);
    fclose(src);

    if (!ok) {
        return -1;
    }

    sync();
    return 0;
}

static inline int config_store_mount(config_store_t *cfg,
                                     config_store_log_fn_t log_fn,
                                     void *log_ctx) {
    if (!cfg || !cfg->mount_point || !cfg->device_path) {
        return -1;
    }

    if (mkdir(cfg->mount_point, 0777) < 0 && errno != EEXIST) {
        config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Failed to create %s: %s", cfg->mount_point, strerror(errno), NULL);
        return -1;
    }

    if (config_store_is_mount_active(cfg->mount_point)) {
        cfg->mount_active = 1;
        return 0;
    }

    if (!config_store_path_exists(cfg->device_path)) {
        config_store_logf(log_fn, log_ctx, "[INIT] [WARN] %s not present; config persistence not mounted.", cfg->device_path, NULL, NULL);
        cfg->mount_active = 0;
        return -1;
    }

    if (mount(cfg->device_path, cfg->mount_point, "ext4", MS_NOATIME, "") < 0) {
        config_store_logf(log_fn,
                          log_ctx,
                          "[INIT] [WARN] Failed to mount %s at %s: %s",
                          cfg->device_path,
                          cfg->mount_point,
                          strerror(errno));
        cfg->mount_active = 0;
        return -1;
    }

    config_store_logf(log_fn,
                      log_ctx,
                      "[INIT] Mounted %s at %s for persistent config storage.",
                      cfg->device_path,
                      cfg->mount_point,
                      NULL);
    sync();
    cfg->mount_active = 1;
    return 0;
}

static inline int config_store_write_flag(config_store_t *cfg,
                                          int enabled,
                                          config_store_log_fn_t log_fn,
                                          void *log_ctx) {
    if (!cfg || !cfg->file_path) {
        return -1;
    }

    char merged[4096];
    size_t merged_len = 0;
    int replaced = 0;

    FILE *in = fopen(cfg->file_path, "r");
    if (in) {
        char line[256];
        while (fgets(line, sizeof(line), in) != NULL) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
                line[len - 1] = '\0';
                --len;
            }

            char *p = line;
            while (*p == ' ' || *p == '\t') {
                ++p;
            }

            char out_line[256];
            out_line[0] = '\0';

            if (strncmp(p, "config_enabled", 14) == 0) {
                const char *q = p + 14;
                while (*q == ' ' || *q == '\t') {
                    ++q;
                }
                if (*q == '=') {
                    snprintf(out_line, sizeof(out_line), "config_enabled=%d", enabled ? 1 : 0);
                    replaced = 1;
                }
            }

            if (!out_line[0]) {
                if ((p[0] == '0' || p[0] == '1') && p[1] == '\0') {
                    snprintf(out_line, sizeof(out_line), "config_enabled=%d", enabled ? 1 : 0);
                    replaced = 1;
                } else {
                    snprintf(out_line, sizeof(out_line), "%s", line);
                }
            }

            size_t out_len = strlen(out_line);
            if (merged_len + out_len + 1 >= sizeof(merged)) {
                fclose(in);
                config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Config merge overflow for %s", cfg->file_path, NULL, NULL);
                return -1;
            }

            memcpy(merged + merged_len, out_line, out_len);
            merged_len += out_len;
            merged[merged_len++] = '\n';
        }
        fclose(in);
    }

    if (!replaced) {
        char line[64];
        int n = snprintf(line, sizeof(line), "config_enabled=%d\n", enabled ? 1 : 0);
        if (n <= 0 || merged_len + (size_t)n >= sizeof(merged)) {
            config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Config append overflow for %s", cfg->file_path, NULL, NULL);
            return -1;
        }
        memcpy(merged + merged_len, line, (size_t)n);
        merged_len += (size_t)n;
    }

    merged[merged_len] = '\0';

    FILE *fp = fopen(cfg->file_path, "w");
    if (!fp) {
        config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Failed to open %s for write: %s", cfg->file_path, strerror(errno), NULL);
        return -1;
    }

    if (fwrite(merged, 1, merged_len, fp) != merged_len) {
        config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Failed to write %s: %s", cfg->file_path, strerror(errno), NULL);
        fclose(fp);
        return -1;
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    sync();
    return 0;
}

static inline int config_store_read_flag(const config_store_t *cfg) {
    if (!cfg || !cfg->file_path) {
        return 0;
    }

    FILE *fp = fopen(cfg->file_path, "r");
    if (!fp) {
        return 0;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }

        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') {
            continue;
        }

        if (strncmp(p, "config_enabled", 14) == 0) {
            char *q = p + 14;
            while (*q == ' ' || *q == '\t') {
                ++q;
            }
            if (*q == '=') {
                ++q;
                while (*q == ' ' || *q == '\t') {
                    ++q;
                }
                int v = (*q == '1') ? 1 : 0;
                fclose(fp);
                return v;
            }
        }

        if ((p[0] == '0' || p[0] == '1') &&
            (p[1] == '\0' || p[1] == '\n' || p[1] == '\r')) {
            int v = (p[0] == '1') ? 1 : 0;
            fclose(fp);
            return v;
        }
    }

    fclose(fp);
    return 0;
}

static inline int config_store_ensure(config_store_t *cfg,
                                      int *enabled_out,
                                      config_store_log_fn_t log_fn,
                                      void *log_ctx) {
    if (enabled_out) {
        *enabled_out = 0;
    }

    if (!cfg || !cfg->file_path) {
        return -1;
    }

    (void)config_store_mount(cfg, log_fn, log_ctx);

    if (!config_store_path_exists(cfg->file_path)) {
        FILE *fp = fopen(cfg->file_path, "w");
        if (!fp) {
            config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Could not create default %s.", cfg->file_path, NULL, NULL);
            return -1;
        }

        const char *default_cfg =
            "usb_playback_channels=2\n"
            "usb_capture_channels=2\n"
            "usb_sample_rate=44100\n"
            "usb_sample_size=2\n";

        size_t want = strlen(default_cfg);
        if (fwrite(default_cfg, 1, want, fp) != want) {
            fclose(fp);
            config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Could not create default %s.", cfg->file_path, NULL, NULL);
            return -1;
        }
        fflush(fp);
        fsync(fileno(fp));
        fclose(fp);
        sync();
        config_store_logf(log_fn, log_ctx, "[INIT] Created default persistent config at %s", cfg->file_path, NULL, NULL);
    }

    if (cfg->staging_file_path && !config_store_path_exists(cfg->staging_file_path)) {
        if (config_store_copy_file(cfg->file_path, cfg->staging_file_path) < 0) {
            config_store_logf(log_fn,
                              log_ctx,
                              "[INIT] [WARN] Could not seed staging config at %s.",
                              cfg->staging_file_path,
                              NULL,
                              NULL);
            return -1;
        }
        config_store_logf(log_fn,
                          log_ctx,
                          "[INIT] Created staging config at %s",
                          cfg->staging_file_path,
                          NULL,
                          NULL);
    }

    if (enabled_out) {
        *enabled_out = config_store_read_flag(cfg);
    }

    cfg->mount_active = config_store_is_mount_active(cfg->mount_point);
    return 0;
}

static inline int config_store_refresh_mount_state(config_store_t *cfg) {
    if (!cfg) {
        return 0;
    }

    cfg->mount_active = config_store_is_mount_active(cfg->mount_point);
    return cfg->mount_active;
}

#endif
