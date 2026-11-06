#include <sys/mount.h>
#include <sys/stat.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <cassert>

static bool shortWrite = false;

static ssize_t testWrite(int fd, const void *value, size_t length) {
    return write(fd, value, shortWrite && length > 0 ? length - 1 : length);
}

#define write testWrite
#include "system.hpp"
#undef write

int main() {
    char path[] = "/tmp/audiox-system-test-XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);

    assert(writeSysNode(path, "AudioX\n") == RET_OK);
    char value[32] = {};
    assert(read(fd, value, sizeof(value)) == 7);
    assert(strcmp(value, "AudioX\n") == 0);

    assert(writeSysNodeU32(path, 48000) == RET_OK);
    assert(lseek(fd, 0, SEEK_SET) == 0);
    memset(value, 0, sizeof(value));
    assert(read(fd, value, sizeof(value)) == 6);
    assert(strcmp(value, "48000\n") == 0);

    shortWrite = true;
    assert(writeSysNode(path, "AudioX\n") == RET_ERR);
    assert(errno == EIO);
    shortWrite = false;

    assert(writeSysNode("/dev/full", "AudioX\n") == RET_ERR);
    assert(errno == ENOSPC);

    assert(close(fd) == 0);
    assert(unlink(path) == 0);
    assert(writeSysNode(path, "AudioX\n") == RET_ERR);
    assert(errno == ENOENT);

    puts("system node write tests passed");
    return 0;
}
