#include "audio/context.hpp"

#include <time.h>
#include <unistd.h>

namespace {

static void *audioProcessingThreadMain(void *arg) {
    AudioContext *ctx = reinterpret_cast<AudioContext *>(arg);
    if (!ctx) {
        return nullptr;
    }

    uint32_t lastGraphGeneration = 0;

    while (ctx->processingThreadRun) {
        {
            std::lock_guard<std::mutex> lock(ctx->routingGraphMutex);
            if (ctx->routingGraph.generation != lastGraphGeneration) {
                lastGraphGeneration = ctx->routingGraph.generation;
                if (lastGraphGeneration > 0) {
                    printf("[AUDIO] [INFO] processing thread observed graph generation %u (%u thing(s), %u edge(s))\n",
                           (unsigned)lastGraphGeneration,
                           (unsigned)ctx->routingGraph.thingCount,
                           (unsigned)ctx->routingGraph.edgeCount);
                }
            }
        }

        // DSP graph execution comes next; for now the thread tracks the compiled graph state.
        usleep(2000);
    }

    return nullptr;
}

} // namespace

int AudioContext::setupThreads() {
    if (processingThreadStarted) {
        return RET_OK;
    }

    int initGraphRc = forceRescan();
    if (initGraphRc == RET_ERR) {
        return RET_ERR;
    }

    processingThreadRun = 1;
    if (pthread_create(&processingThread, nullptr, audioProcessingThreadMain, this) != 0) {
        processingThreadRun = 0;
        return RET_ERR;
    }

    processingThreadStarted = 1;
    return (initGraphRc == RET_WARN) ? RET_WARN : RET_OK;
}
