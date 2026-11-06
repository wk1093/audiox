#include "hardware_telemetry.hpp"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <cstring>
#include <cstdlib>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
bool failIoctl = false, malformed = false;
uint32_t firmwareFlags = 0;
int firmwareOpen(const char *path, int flags) {
    if (strcmp(path, "/dev/vcio") != 0) return ::open(path, flags);
    assert((flags & O_RDWR) != 0);
    return 123;
}
int firmwareClose(int fd) { return fd == 123 ? 0 : ::close(fd); }
int firmwareIoctl(int fd, unsigned long command, void *buffer) {
    assert(fd == 123 && command == _IOWR(100, 0, char *));
    auto *message = static_cast<uint32_t *>(buffer);
    assert(message[0] == 28 && message[1] == 0);
    assert(message[2] == 0x00030046 && message[3] == 4 && message[4] == 0 && message[6] == 0);
    if (failIoctl) { errno = EIO; return -1; }
    message[1] = 0x80000000;
    message[4] = malformed ? 0x80000008 : 0x80000004;
    message[5] = firmwareFlags;
    return 0;
}
}

#define open firmwareOpen
#define close firmwareClose
#define ioctl firmwareIoctl
#include "../src/system/hardware_telemetry.cpp"
#undef open
#undef close
#undef ioctl

int main() {
    using namespace audiox::system;
    char path[] = "/tmp/audiox-small-file-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    char buffer[4]{};
    size_t length = 99;
    assert(!readSmallFile(path, buffer, sizeof(buffer), &length));
    assert(length == 0 && buffer[0] == '\0');
    assert(write(fd, "abc", 3) == 3);
    assert(readSmallFile(path, buffer, sizeof(buffer), &length));
    assert(length == 3 && !strcmp(buffer, "abc"));
    assert(write(fd, "d", 1) == 1);
    assert(!readSmallFile(path, buffer, sizeof(buffer), &length));
    assert(!readSmallFile(path, buffer, 1));
    assert(!readSmallFile(path, buffer, 0));
    assert(::close(fd) == 0 && unlink(path) == 0);
    uint32_t flags = 99;
    assert(readFirmwareThrottleFlags(&flags) && flags == 0);
    firmwareFlags = (1U << 0) | (1U << 18);
    assert(readFirmwareThrottleFlags(&flags) && flags == firmwareFlags);
    ThrottleFlags undervoltage, throttling, softLimit;
    parseFirmwareThrottleFlags(flags, &undervoltage, &throttling, &softLimit);
    assert(undervoltage.current == 1 && throttling.sinceBoot == 1);
    failIoctl = true;
    assert(!readFirmwareThrottleFlags(&flags));
    failIoctl = false;
    malformed = true;
    assert(!readFirmwareThrottleFlags(&flags));
}
