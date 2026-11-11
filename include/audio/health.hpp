#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <time.h>

namespace audiox {

inline uint64_t healthMonotonicMs() {
    timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

class AudioHealth {
public:
    static constexpr uint64_t kGraceMs = 60000;
    static constexpr uint64_t kDeviceGraceMs = 30000;
    static constexpr uint64_t kBufferGraceMs = 10000;
    static constexpr uint64_t kWindowMs = 30000;
    static constexpr uint64_t kCooldownMs = 120000;
    static constexpr size_t kThreshold = 5;

    explicit AudioHealth(uint64_t started = healthMonotonicMs()) : graceUntil_(started + kGraceMs) {}

    void devicesChanged(uint64_t now = healthMonotonicMs()) {
        extendGrace(now + kDeviceGraceMs);
    }

    void bufferChanged(uint64_t now = healthMonotonicMs()) {
        extendGrace(now + kBufferGraceMs);
    }
    void workerRestarted(uint64_t now = healthMonotonicMs()) {
        extendGrace(now + kGraceMs);
    }

    // Called by the audio processing thread: bounded atomics only, no allocation or log parsing.
    bool recordXrun(uint64_t now = healthMonotonicMs()) {
        total_.fetch_add(1, std::memory_order_relaxed);
        lastXrunAt_.store(now, std::memory_order_release);
        const uint64_t grace = graceUntil_.load(std::memory_order_relaxed);
        if (now < grace) return false;
        const size_t slot = next_.fetch_add(1, std::memory_order_relaxed) % (kThreshold - 1);
        const uint64_t oldest = recent_[slot].exchange(now, std::memory_order_relaxed);
        if (!oldest || oldest < grace || now < oldest || now - oldest > kWindowMs) return false;
        uint64_t previous = warningAt_.load(std::memory_order_relaxed);
        if (previous && now - previous < kCooldownMs) return false;
        return warningAt_.compare_exchange_strong(previous, now, std::memory_order_release);
    }

    uint64_t total() const { return total_.load(std::memory_order_relaxed); }
    void recordWorkerXruns(uint64_t count, uint64_t now = healthMonotonicMs()) {
        const uint64_t recent = std::min<uint64_t>(count, kThreshold);
        total_.fetch_add(count - recent, std::memory_order_relaxed);
        for (uint64_t i = 0; i < recent; ++i) recordXrun(now);
    }
    uint64_t warningAt() const { return warningAt_.load(std::memory_order_acquire); }
    bool warningActive(uint64_t now = healthMonotonicMs()) const {
        const uint64_t warning = warningAt();
        const uint64_t lastXrun = lastXrunAt_.load(std::memory_order_acquire);
        const uint64_t grace = graceUntil_.load(std::memory_order_relaxed);
        return warning && warning >= grace && now >= lastXrun && now - lastXrun <= kWindowMs;
    }

private:
    void extendGrace(uint64_t until) {
        uint64_t previous = graceUntil_.load(std::memory_order_relaxed);
        while (previous < until &&
               !graceUntil_.compare_exchange_weak(previous, until, std::memory_order_relaxed)) {}
    }

    std::atomic<uint64_t> graceUntil_, total_{0}, warningAt_{0}, lastXrunAt_{0};
    std::atomic<size_t> next_{0};
    std::atomic<uint64_t> recent_[kThreshold - 1]{};
};

class AdaptiveAudioBuffer {
public:
    static constexpr uint32_t kMaxPeriodFrames = 1024;
    static constexpr uint64_t kXrunWindowMs = 30000;
    static constexpr uint64_t kStartupGraceMs = 60000;
    static constexpr uint64_t kShiftGraceMs = 10000;
    static constexpr uint64_t kDownshiftSettleMs = 1000;
    static constexpr uint64_t kQuietPeriodMs = 30000;
    static constexpr uint64_t kBusyQuietPeriodMs = 60000;
    static constexpr uint64_t kDownshiftRetryWindowMs = 600000;
    static constexpr uint64_t kDownshiftBackoffMs[] = {
        0, 300000, 600000, 900000
    };

    explicit AdaptiveAudioBuffer(uint64_t started = healthMonotonicMs())
        : instanceId_(started), startupGraceUntil_(started + kStartupGraceMs),
          graceUntil_(startupGraceUntil_) {}

    // Audio-thread only; successful opens extend settling past the reconfiguration work.
    void bufferChanged(uint64_t now = healthMonotonicMs()) {
        graceUntil_ = std::max(graceUntil_, now + kShiftGraceMs);
        if (downshiftRetryActive(now))
            downshiftSettleUntil_ = now + kDownshiftSettleMs;
        lastXrunAt_ = now;
        clearXrunWindow();
    }

    uint32_t level() const { return level_.load(std::memory_order_acquire); }
    uint64_t instanceId() const {
        const uint64_t remote = remoteInstance_.load(std::memory_order_acquire);
        return remote ? remote : instanceId_;
    }

    void mirrorWorker(uint64_t instance, uint32_t level, uint32_t streams,
                      uint32_t period, uint32_t buffer, uint64_t change,
                      uint64_t increase, uint32_t delay, uint32_t failures) {
        level_.store(level, std::memory_order_relaxed);
        publishObserved(streams, period, buffer);
        changeId_.store(change, std::memory_order_relaxed);
        increaseId_.store(increase, std::memory_order_relaxed);
        remoteDelay_.store(delay, std::memory_order_relaxed);
        downshiftFailures_.store(failures, std::memory_order_relaxed);
        remoteInstance_.store(instance, std::memory_order_release);
    }

    uint32_t targetPeriodFrames(uint32_t baseFrames) const {
        uint32_t frames = baseFrames;
        const uint32_t currentLevel = level();
        for (uint32_t i = 0; i < currentLevel && frames < kMaxPeriodFrames; ++i)
            frames = frames > kMaxPeriodFrames / 2 ? kMaxPeriodFrames : frames * 2;
        return frames > kMaxPeriodFrames ? kMaxPeriodFrames : frames;
    }

    uint32_t maxLevel(uint32_t baseFrames) const {
        if (!baseFrames || baseFrames >= kMaxPeriodFrames) return 0;
        uint32_t frames = baseFrames;
        uint32_t count = 0;
        while (frames < kMaxPeriodFrames) {
            frames = frames > kMaxPeriodFrames / 2 ? kMaxPeriodFrames : frames * 2;
            ++count;
        }
        return count;
    }

    bool recordXrun(uint64_t now, uint32_t baseFrames) {
        if (now < startupGraceUntil_) return false;
        lastXrunAt_ = now;
        hasXrun_ = true;
        if (now < graceUntil_ &&
            (!downshiftRetryActive(now) || now < downshiftSettleUntil_)) return false;
        xrunTimes_[xrunIndex_++ % kXrunsToIncrease] = now;
        uint32_t recentXruns = 0;
        for (uint64_t time : xrunTimes_)
            if (time && time <= now && now - time <= kXrunWindowMs) ++recentXruns;
        recentBurstCount_.store(recentXruns, std::memory_order_relaxed);
        if (recentXruns < kXrunsToIncrease) return false;

        uint32_t current = level();
        if (current >= maxLevel(baseFrames)) return false;

        level_.store(current + 1, std::memory_order_release);
        changeId_.fetch_add(1, std::memory_order_relaxed);
        increaseId_.fetch_add(1, std::memory_order_release);
        if (pendingDownshiftFromLevel_ == current + 1 && downshiftRetryActive(now)) {
            const uint32_t failures = downshiftFailures_.load(std::memory_order_relaxed);
            if (failures < 3) downshiftFailures_.store(failures + 1, std::memory_order_release);
        }
        pendingDownshiftFromLevel_ = 0;
        bufferChanged(now);
        return true;
    }

    bool tick(uint64_t now) {
        uint32_t current = level();
        if (!current || !hasXrun_ || now < graceUntil_ || now < lastXrunAt_) return false;
        const uint64_t quietPeriod =
            recentBurstCount_.load(std::memory_order_relaxed) >= kXrunsToIncrease
            ? kBusyQuietPeriodMs : kQuietPeriodMs;
        const uint32_t failures = downshiftFailures_.load(std::memory_order_relaxed);
        const uint64_t requiredQuietMs = std::max(
            quietPeriod, kDownshiftBackoffMs[failures]);
        if (now - lastXrunAt_ < requiredQuietMs) return false;

        level_.store(current - 1, std::memory_order_release);
        changeId_.fetch_add(1, std::memory_order_relaxed);
        pendingDownshiftFromLevel_ = current;
        lastDownshiftAt_ = now;
        lastXrunAt_ = now;
        recentBurstCount_.store(0, std::memory_order_relaxed);
        bufferChanged(now);
        return true;
    }

    void publishObserved(uint32_t streams, uint32_t largestPeriodFrames,
                         uint32_t largestBufferFrames) {
        activeStreams_.store(streams, std::memory_order_relaxed);
        largestPeriodFrames_.store(largestPeriodFrames, std::memory_order_relaxed);
        largestBufferFrames_.store(largestBufferFrames, std::memory_order_relaxed);
    }

    uint64_t changeId() const { return changeId_.load(std::memory_order_acquire); }
    uint64_t increaseId() const { return increaseId_.load(std::memory_order_acquire); }
    uint32_t activeStreams() const { return activeStreams_.load(std::memory_order_relaxed); }
    uint32_t largestPeriodFrames() const { return largestPeriodFrames_.load(std::memory_order_relaxed); }
    uint32_t largestBufferFrames() const { return largestBufferFrames_.load(std::memory_order_relaxed); }
    uint32_t downshiftFailures() const {
        return downshiftFailures_.load(std::memory_order_acquire);
    }
    uint32_t downshiftDelayMs() const {
        if (remoteInstance_.load(std::memory_order_acquire))
            return remoteDelay_.load(std::memory_order_relaxed);
        if (!level()) return 0;
        const uint64_t quietPeriod =
            recentBurstCount_.load(std::memory_order_relaxed) >= kXrunsToIncrease
            ? kBusyQuietPeriodMs : kQuietPeriodMs;
        const uint32_t failures = downshiftFailures_.load(std::memory_order_relaxed);
        return static_cast<uint32_t>(std::max(quietPeriod,
                                              kDownshiftBackoffMs[failures]));
    }

private:
    static constexpr uint32_t kXrunsToIncrease = 5;

    bool downshiftRetryActive(uint64_t now) const {
        return pendingDownshiftFromLevel_ && now >= lastDownshiftAt_ &&
            now - lastDownshiftAt_ <= kDownshiftRetryWindowMs;
    }

    void clearXrunWindow() {
        xrunIndex_ = 0;
        for (uint64_t &time : xrunTimes_) time = 0;
    }

    const uint64_t instanceId_;
    std::atomic<uint64_t> remoteInstance_{0};
    std::atomic<uint32_t> remoteDelay_{0};
    const uint64_t startupGraceUntil_;
    uint64_t graceUntil_;
    std::atomic<uint32_t> level_{0};
    std::atomic<uint64_t> changeId_{0}, increaseId_{0};
    std::atomic<uint32_t> activeStreams_{0}, largestPeriodFrames_{0}, largestBufferFrames_{0};
    uint64_t xrunTimes_[kXrunsToIncrease]{};
    uint32_t xrunIndex_ = 0, pendingDownshiftFromLevel_ = 0;
    std::atomic<uint32_t> recentBurstCount_{0}, downshiftFailures_{0};
    uint64_t lastXrunAt_ = 0;
    uint64_t lastDownshiftAt_ = 0;
    uint64_t downshiftSettleUntil_ = 0;
    bool hasXrun_ = false;
};

inline AudioHealth audioHealth;
inline AdaptiveAudioBuffer adaptiveAudioBuffer;

} // namespace audiox
