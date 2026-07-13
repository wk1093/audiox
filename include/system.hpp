#pragma once

#include "defs.hpp"

#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>

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

static inline int writeSysNode(const char *path, const char *value) {
    if (!path || !value) {
        return RET_ERR;
    }

    int fd = open(path, O_WRONLY | O_TRUNC);
    if (fd < 0) {
        return RET_ERR;
    }

    ssize_t n = write(fd, value, strlen(value));
    close(fd);
    if (n < 0 || (size_t)n != strlen(value)) {
        return RET_ERR;
    }
    return RET_OK;
}

static inline int writeSysNodeU32(const char *path, uint32_t value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%u\n", (unsigned)value);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        return RET_ERR;
    }
    return writeSysNode(path, buf);
}

static inline int readSysNodeU32(const char *path, uint32_t *value_out) {
    if (!path || !value_out) {
        return RET_ERR;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return RET_ERR;
    }

    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return RET_ERR;
    }

    buf[n] = '\0';
    char *endp = NULL;
    unsigned long v = strtoul(buf, &endp, 10);
    if (endp == buf) {
        return RET_ERR;
    }

    *value_out = (uint32_t)v;
    return RET_OK;
}