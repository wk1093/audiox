#pragma once

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <string>

struct AudioContext;

namespace audiox::worker {

class RecoveryPolicy {
public:
    static constexpr uint32_t kFailuresBeforeBypass = 3;
    static constexpr uint64_t kStableMs = 60000;
    uint32_t failures = 0;
    uint64_t retryAt = 0;
    bool pluginsDisabled = false;

    void failed(uint64_t now) {
        if (failures < 10) ++failures;
        if (failures >= kFailuresBeforeBypass) pluginsDisabled = true;
        const uint32_t shift = failures > 5 ? 5 : failures - 1;
        retryAt = now + (250U << shift);
    }
    void stable(uint64_t now, uint64_t started) {
        if (now >= started && now - started >= kStableMs) failures = 0;
    }
    void manual(bool enablePlugins) {
        failures = 0;
        retryAt = 0;
        if (enablePlugins) pluginsDisabled = false;
    }
};

int run(int fd, int parentPid);

} // namespace audiox::worker

class AudioWorkerSupervisor {
public:
    explicit AudioWorkerSupervisor(AudioContext &context,
                                   const char *path = "/sbin/audiox-audio-worker");
    ~AudioWorkerSupervisor();
    bool start();
    int restart(bool enablePlugins);
    std::string statusJson() const;
private:
    struct State;
    State *state_;
    pthread_t thread_{};
    bool started_{false};
    static void *threadMain(void *);
};
