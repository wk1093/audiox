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
                                     const char *file_path) {
    if (!cfg) {
        return;
    }

    cfg->mount_point = mount_point;
    cfg->device_path = device_path;
    cfg->file_path = file_path;
    cfg->mount_active = config_store_is_mount_active(mount_point);
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

    FILE *fp = fopen(cfg->file_path, "w");
    if (!fp) {
        config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Failed to open %s for write: %s", cfg->file_path, strerror(errno), NULL);
        return -1;
    }

    if (fprintf(fp, "%d\n", enabled ? 1 : 0) < 0) {
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

    int c = fgetc(fp);
    fclose(fp);
    return (c == '1') ? 1 : 0;
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
        if (config_store_write_flag(cfg, 0, log_fn, log_ctx) < 0) {
            config_store_logf(log_fn, log_ctx, "[INIT] [WARN] Could not create default %s.", cfg->file_path, NULL, NULL);
            return -1;
        }
        config_store_logf(log_fn, log_ctx, "[INIT] Created default persistent config at %s", cfg->file_path, NULL, NULL);
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
