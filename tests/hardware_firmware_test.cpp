#include "hardware_telemetry.hpp"
#include <cassert>
#include <cerrno>
#include <fcntl.h>
#include <cstring>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {
bool failIoctl = false, malformed = false;
uint32_t firmwareFlags = 0;
int firmwareOpen(const char *path, int flags) {
    assert(strcmp(path, "/dev/vcio") == 0);
    assert((flags & O_RDWR) != 0);
    return 123;
}
int firmwareClose(int fd) { assert(fd == 123); return 0; }
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
