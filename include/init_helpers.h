#ifndef INIT_HELPERS_H
#define INIT_HELPERS_H

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

static inline int ensure_dir(const char *path, mode_t mode) {
    if (mkdir(path, mode) < 0 && errno != EEXIST) {
        printf("[INIT] [ERR] mkdir failed '%s': %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static inline int mount_if_needed(const char *src, const char *dst, const char *type) {
    if (mount(src, dst, type, 0, NULL) < 0 && errno != EBUSY) {
        printf("[INIT] [ERR] mount %s on %s failed: %s\n", type, dst, strerror(errno));
        return -1;
    }
    return 0;
}

static inline int mount_basic_filesystems(void) {
    if (mount_if_needed("sysfs", "/sys", "sysfs") < 0) return -1;
    if (mount_if_needed("proc", "/proc", "proc") < 0) return -1;
    if (mount_if_needed("devtmpfs", "/dev", "devtmpfs") < 0) return -1;
    if (ensure_dir("/sys/kernel/config", 0755) < 0) return -1;
    if (mount_if_needed("configfs", "/sys/kernel/config", "configfs") < 0) return -1;
    return 0;
}

static inline int load_module(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[INIT] [ERR] Failed to open module %s: %s\n", path, strerror(errno));
        return -1;
    }

    int rc = syscall(SYS_finit_module, fd, "", 0);
    int saved_errno = errno;
    close(fd);

    if (rc < 0 && saved_errno != EEXIST) {
        if (saved_errno == ENOENT) {
            printf("[INIT] [ERR] Syscall failed for %s: %s (errno %d, likely missing dependency module)\n", path, strerror(saved_errno), saved_errno);
        } else {
            printf("[INIT] [ERR] Syscall failed for %s: %s (errno %d)\n", path, strerror(saved_errno), saved_errno);
        }
        return -1;
    }

    return 0;
}

static inline int write_sys_node(const char *path, const char *value) {
    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        printf("[INIT] [ERR] Path activation failed '%s': %s\n", path, strerror(errno));
        return -1;
    }

    ssize_t len = (ssize_t)strlen(value);
    if (write(fd, value, (size_t)len) != len) {
        printf("[INIT] [ERR] Write failed on node '%s': %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

#endif
