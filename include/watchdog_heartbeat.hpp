#pragma once

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// Long main-thread transfers must report progress, not disable the watchdog.
inline void audioxWatchdogHeartbeat() {
    static const int fd = []() {
        const char *value = getenv("AUDIOX_WATCHDOG_FD");
        if (!value || !value[0]) return -1;
        char *end = nullptr;
        long result = strtol(value, &end, 10);
        return end && *end == '\0' && result >= 3 && result <= 1024 ? (int)result : -1;
    }();
    if (fd < 0) return;
    static uint64_t last = 0;
    static uint64_t sequence = 0;
    timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) < 0) {
        perror("[INIT] watchdog clock");
        return;
    }
    uint64_t ms = (uint64_t)now.tv_sec * 1000 + (uint64_t)now.tv_nsec / 1000000;
    if (ms - last < 500) return;
    last = ms;
    ++sequence;
    if (write(fd, &sequence, sizeof(sequence)) != sizeof(sequence)) {
        perror("[INIT] watchdog heartbeat");
    }
}
