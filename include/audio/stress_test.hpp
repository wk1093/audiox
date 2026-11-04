#pragma once

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <pthread.h>
#include <unistd.h>

namespace audiox {

class AudioStressTest {
public:
    static constexpr uint32_t kMaxDurationSeconds = 30;
    static constexpr uint32_t kMaxAudioWorkMsPerBlock = 20;
    static constexpr uint32_t kMaxWorkers = 8;

    enum Mode : uint32_t {
        Idle = 0,
        AudioThread = 1,
        CpuContention = 2,
    };

    AudioStressTest() = default;
    AudioStressTest(const AudioStressTest &) = delete;
    AudioStressTest &operator=(const AudioStressTest &) = delete;

    ~AudioStressTest() { stop(); }

    int startAudioThread(uint32_t durationSeconds, uint32_t workMsPerBlock) {
        if (durationSeconds == 0 || durationSeconds > kMaxDurationSeconds ||
            workMsPerBlock == 0 || workMsPerBlock > kMaxAudioWorkMsPerBlock)
            return -EINVAL;

        pthread_mutex_lock(&mutex_);
        reapCpuWorkersLocked();
        if (mode_.load(std::memory_order_acquire) != Idle) {
            pthread_mutex_unlock(&mutex_);
            return -EBUSY;
        }
        workNsPerBlock_.store(static_cast<uint64_t>(workMsPerBlock) * 1000000ULL,
                              std::memory_order_relaxed);
        expiresAtNs_.store(nowNs() + static_cast<uint64_t>(durationSeconds) * 1000000000ULL,
                           std::memory_order_relaxed);
        mode_.store(AudioThread, std::memory_order_release);
        pthread_mutex_unlock(&mutex_);
        return 0;
    }

    int startCpuContention(uint32_t durationSeconds) {
        if (durationSeconds == 0 || durationSeconds > kMaxDurationSeconds)
            return -EINVAL;

        pthread_mutex_lock(&mutex_);
        reapCpuWorkersLocked();
        if (mode_.load(std::memory_order_acquire) != Idle) {
            pthread_mutex_unlock(&mutex_);
            return -EBUSY;
        }

        const long onlineCpus = sysconf(_SC_NPROCESSORS_ONLN);
        const uint32_t requested = onlineCpus > 0
            ? static_cast<uint32_t>(std::min<long>(onlineCpus, kMaxWorkers)) : 1U;
        const uint32_t workerCount = requested ? requested : 1U;
        workerCount_.store(workerCount, std::memory_order_relaxed);
        workerStop_.store(false, std::memory_order_relaxed);
        expiresAtNs_.store(nowNs() + static_cast<uint64_t>(durationSeconds) * 1000000000ULL,
                           std::memory_order_relaxed);
        workersRemaining_.store(workerCount, std::memory_order_relaxed);
        mode_.store(CpuContention, std::memory_order_release);

        for (uint32_t i = 0; i < workerCount; ++i) {
            const int rc = pthread_create(&workers_[i], nullptr, cpuWorkerMain, this);
            if (rc == 0) continue;

            mode_.store(Idle, std::memory_order_release);
            workerStop_.store(true, std::memory_order_release);
            for (uint32_t joined = 0; joined < i; ++joined)
                pthread_join(workers_[joined], nullptr);
            workerCount_.store(0, std::memory_order_relaxed);
            workersRemaining_.store(0, std::memory_order_relaxed);
            pthread_mutex_unlock(&mutex_);
            return -rc;
        }
        pthread_mutex_unlock(&mutex_);
        return 0;
    }

    void processAudioBlock() {
        if (mode_.load(std::memory_order_acquire) != AudioThread) return;
        const uint64_t now = nowNs();
        const uint64_t expiresAt = expiresAtNs_.load(std::memory_order_relaxed);
        if (now >= expiresAt) {
            uint32_t expected = AudioThread;
            mode_.compare_exchange_strong(expected, Idle, std::memory_order_release);
            return;
        }

        const uint64_t workNs = workNsPerBlock_.load(std::memory_order_relaxed);
        burnCpuUntil(std::min(expiresAt, now + workNs), &mode_, AudioThread);
        if (nowNs() >= expiresAt) {
            uint32_t expected = AudioThread;
            mode_.compare_exchange_strong(expected, Idle, std::memory_order_release);
        }
    }

    void stop() {
        pthread_mutex_lock(&mutex_);
        mode_.store(Idle, std::memory_order_release);
        workerStop_.store(true, std::memory_order_release);
        reapCpuWorkersLocked();
        pthread_mutex_unlock(&mutex_);
    }

    uint32_t mode() const { return mode_.load(std::memory_order_acquire); }
    uint32_t workerCount() const {
        return mode() == CpuContention ? workerCount_.load(std::memory_order_relaxed) : 0;
    }

    uint32_t remainingMs() const {
        if (mode() == Idle) return 0;
        const uint64_t now = nowNs();
        const uint64_t expiry = expiresAtNs_.load(std::memory_order_relaxed);
        return now >= expiry ? 0U : static_cast<uint32_t>((expiry - now + 999999ULL) / 1000000ULL);
    }

    static const char *modeName(uint32_t mode) {
        switch (mode) {
        case AudioThread: return "audio";
        case CpuContention: return "cpu";
        default: return "idle";
        }
    }

private:
    static uint64_t nowNs() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    static void burnCpuUntil(uint64_t deadlineNs, const std::atomic<uint32_t> *mode,
                             uint32_t expectedMode, const std::atomic<bool> *stop = nullptr) {
        volatile uint64_t value = 0x9e3779b97f4a7c15ULL;
        while (nowNs() < deadlineNs &&
               (!mode || mode->load(std::memory_order_relaxed) == expectedMode) &&
               (!stop || !stop->load(std::memory_order_relaxed))) {
            for (uint32_t i = 0; i < 4096; ++i)
                value = value * 6364136223846793005ULL + 1442695040888963407ULL;
        }
        result_.store(value, std::memory_order_relaxed);
    }

    static void *cpuWorkerMain(void *arg) {
        auto *self = static_cast<AudioStressTest *>(arg);
        const uint64_t expiry = self->expiresAtNs_.load(std::memory_order_relaxed);
        burnCpuUntil(expiry, nullptr, Idle, &self->workerStop_);
        if (self->workersRemaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            uint32_t expected = CpuContention;
            self->mode_.compare_exchange_strong(expected, Idle, std::memory_order_release);
        }
        return nullptr;
    }

    void reapCpuWorkersLocked() {
        const uint32_t workerCount = workerCount_.load(std::memory_order_relaxed);
        if (!workerCount) return;
        if (mode_.load(std::memory_order_acquire) == CpuContention) return;
        for (uint32_t i = 0; i < workerCount; ++i)
            pthread_join(workers_[i], nullptr);
        workerCount_.store(0, std::memory_order_relaxed);
    }

    pthread_mutex_t mutex_ = PTHREAD_MUTEX_INITIALIZER;
    pthread_t workers_[kMaxWorkers]{};
    std::atomic<uint32_t> workerCount_{0};
    std::atomic<uint32_t> mode_{Idle};
    std::atomic<uint32_t> workersRemaining_{0};
    std::atomic<uint64_t> expiresAtNs_{0};
    std::atomic<uint64_t> workNsPerBlock_{0};
    inline static std::atomic<uint64_t> result_{0};
    std::atomic<bool> workerStop_{false};
};

inline AudioStressTest audioStressTest;

} // namespace audiox
