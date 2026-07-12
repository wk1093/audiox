#include "audio/context.hpp"

#include <time.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config/context.hpp"

namespace {

static uint64_t audioMonotonicMs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

}

AudioContext::AudioContext(Audiox *context) : app(context) {
    if (app) {
        app->audio = this;
    }

    processingThread = (pthread_t)0;
    processingThreadStarted = 0;
    processingThreadRun = 0;

    nextHotplugScanMs = 0;
    nextHandle = 1;
    deviceGeneration = 0;
    memset(&routingGraph, 0, sizeof(routingGraph));
}

AudioContext::~AudioContext() {
    processingThreadRun = 0;
}

int AudioContext::triggerSfx(const char *sfxPath) {
    if (!sfxPath || !sfxPath[0]) {
        return RET_ERR;
    }

    if (strncmp(sfxPath, SFX_ROOT_DIR "/", strlen(SFX_ROOT_DIR) + 1) != 0) {
        printf("[AUDIO] [WARN] refusing SFX outside %s: %s\n", SFX_ROOT_DIR, sfxPath);
        return RET_ERR;
    }

    struct stat st;
    if (stat(sfxPath, &st) < 0) {
        printf("[AUDIO] [WARN] mapped SFX missing: %s\n", sfxPath);
        return RET_WARN;
    }

    // Placeholder until audio engine routing/playback is fully ported.
    printf("[AUDIO] [WARN] SFX trigger requested but playback is not implemented yet: %s\n", sfxPath);
    return RET_WARN;
}

void AudioContext::poll() {
    uint64_t now = audioMonotonicMs();
    if (now < nextHotplugScanMs) {
        return;
    }

    nextHotplugScanMs = now + 1000ULL;
    (void)rescanDevices();
}

int AudioContext::forceRescan() {
    nextHotplugScanMs = 0;
    int rc = rescanDevices();
    int graphRc = reloadRoutingGraph();
    if (rc == RET_ERR || graphRc == RET_ERR) {
        return RET_ERR;
    }
    if (rc == RET_WARN || graphRc == RET_WARN) {
        return RET_WARN;
    }
    return RET_OK;
}
