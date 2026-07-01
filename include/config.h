#ifndef CONFIG_H
#define CONFIG_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#define CONFIG_MOUNT_POINT "/raspi_audio"
#define CONFIG_DEVICE_PATH "/dev/mmcblk0p2"
#define CONFIG_FILE_PATH CONFIG_MOUNT_POINT "/config.txt"

typedef struct config_ctx {
    int enabled;
    int mount_active;
} config_ctx_t;

static inline int config_path_exists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0) ? 1 : 0;
}

static inline int config_is_mount_active(const char *mount_point) {
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

static inline int config_mount_partition(void) {
    if (mkdir(CONFIG_MOUNT_POINT, 0777) < 0 && errno != EEXIST) {
        fprintf(stderr, "[CONFIG] [WARN] Failed to create %s: %s\n", CONFIG_MOUNT_POINT, strerror(errno));
        return -1;
    }

    if (config_is_mount_active(CONFIG_MOUNT_POINT)) {
        return 0;
    }

    if (!config_path_exists(CONFIG_DEVICE_PATH)) {
        fprintf(stderr, "[CONFIG] [WARN] %s not present; config persistence not mounted.\n", CONFIG_DEVICE_PATH);
        return -1;
    }

    if (mount(CONFIG_DEVICE_PATH, CONFIG_MOUNT_POINT, "ext4", MS_NOATIME, "") < 0) {
        fprintf(stderr, "[CONFIG] [WARN] Failed to mount %s at %s: %s\n",
                CONFIG_DEVICE_PATH, CONFIG_MOUNT_POINT, strerror(errno));
        return -1;
    }

    printf("[CONFIG] Mounted %s at %s for persistent config storage.\n", CONFIG_DEVICE_PATH, CONFIG_MOUNT_POINT);
    sync();
    return 0;
}

static inline int config_write_flag(int enabled) {
    FILE *fp = fopen(CONFIG_FILE_PATH, "w");
    if (!fp) {
        fprintf(stderr, "[CONFIG] [WARN] Failed to open %s for write: %s\n", CONFIG_FILE_PATH, strerror(errno));
        return -1;
    }

    if (fprintf(fp, "%d\n", enabled ? 1 : 0) < 0) {
        fprintf(stderr, "[CONFIG] [WARN] Failed to write %s: %s\n", CONFIG_FILE_PATH, strerror(errno));
        fclose(fp);
        return -1;
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    sync();
    return 0;
}

static inline int config_read_flag(void) {
    FILE *fp = fopen(CONFIG_FILE_PATH, "r");
    if (!fp) {
        return 0;
    }

    int c = fgetc(fp);
    fclose(fp);
    return (c == '1') ? 1 : 0;
}

static inline int config_init(config_ctx_t *cfg) {
    if (!cfg) {
        return -1;
    }

    cfg->enabled = 0;
    cfg->mount_active = 0;

    (void)config_mount_partition();

    if (!config_path_exists(CONFIG_FILE_PATH)) {
        if (config_write_flag(0) < 0) {
            fprintf(stderr, "[CONFIG] [WARN] Could not create default %s.\n", CONFIG_FILE_PATH);
            return -1;
        }
        printf("[CONFIG] Created default persistent config at %s\n", CONFIG_FILE_PATH);
    }

    cfg->enabled = config_read_flag();
    cfg->mount_active = config_is_mount_active(CONFIG_MOUNT_POINT);

    return 0;
}

#endif
