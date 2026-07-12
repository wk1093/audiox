#include "init.hpp"
#include "context.hpp"

#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>

static inline int mountIfNeeded(const char *src, const char *dst, const char *type) {
    if (mount(src, dst, type, 0, NULL) < 0 && errno != EBUSY) {
        printf("[INIT] [ERR] mount %s on %s failed: %s\n", src, dst, strerror(errno));
        return RET_ERR;
    }
    return RET_OK;
}

static inline int ensureDir(const char *path, mode_t mode) {
    if (mkdir(path, mode) < 0 && errno != EEXIST) {
        printf("[INIT] [ERR] mkdir failed '%s': %s\n", path, strerror(errno));
        return RET_ERR;
    }
    return RET_OK;
}

int mountFilesystems() {
    if (mountIfNeeded("sysfs", "/sys", "sysfs") != RET_OK) return RET_ERR;
    if (mountIfNeeded("proc", "/proc", "proc") != RET_OK) return RET_ERR;
    if (mountIfNeeded("devtmpfs", "/dev", "devtmpfs") != RET_OK) return RET_ERR;
    if (ensureDir("/sys/kernel/config", 0755) != RET_OK) return RET_ERR;
    if (mountIfNeeded("configfs", "/sys/kernel/config", "configfs") != RET_OK) return RET_ERR;

    return RET_WARN;
}


int mountRootfs(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }

    if (ensureDir(ROOT_MOUNT_POINT, 0755) != RET_OK) return RET_ERR;
    if (mount(CONFIG_DEVICE_PATH, ROOT_MOUNT_POINT, "ext4", MS_NOATIME, NULL) < 0 && errno != EBUSY) {
        printf("[INIT] [ERR] mount %s -> %s failed: %s\n", CONFIG_DEVICE_PATH, ROOT_MOUNT_POINT, strerror(errno));
        return RET_ERR;
    }

    return RET_WARN;
}