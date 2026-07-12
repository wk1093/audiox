#include "audio/context.hpp"

#include <time.h>
#include <unistd.h>

namespace {

static void *audioProcessingThreadMain(void *arg) {
    AudioContext *ctx = reinterpret_cast<AudioContext *>(arg);
    if (!ctx) {
        return nullptr;
    }

    while (ctx->processingThreadRun) {
        // No DSP graph yet; hotplug and routing polling still run in main poll loop.
        usleep(2000);
    }

    return nullptr;
}

} // namespace

int AudioContext::setupThreads() {
    if (processingThreadStarted) {
        return RET_OK;
    }

    processingThreadRun = 1;
    if (pthread_create(&processingThread, nullptr, audioProcessingThreadMain, this) != 0) {
        processingThreadRun = 0;
        return RET_ERR;
    }

    processingThreadStarted = 1;
    return RET_OK;
}
