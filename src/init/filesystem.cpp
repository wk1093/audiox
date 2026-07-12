#include "init.hpp"
#include "context.hpp"
#include "system.hpp"
#include "config/context.hpp"

#include <stdio.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>


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

    if (ensureDir(SFX_ROOT_DIR, 0755) != RET_OK) {
        return RET_ERR;
    }

    return RET_WARN;
}