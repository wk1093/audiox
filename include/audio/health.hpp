#pragma once

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
    static constexpr uint64_t kGraceMs = 30000;
    static constexpr uint64_t kWindowMs = 30000;
    static constexpr uint64_t kCooldownMs = 120000;
    static constexpr size_t kThreshold = 5;

    explicit AudioHealth(uint64_t started = healthMonotonicMs()) : graceUntil_(started + kGraceMs) {}

    void devicesChanged(uint64_t now = healthMonotonicMs()) {
        graceUntil_.store(now + kGraceMs, std::memory_order_relaxed);
    }

    // Called by the audio processing thread: bounded atomics only, no allocation or log parsing.
    bool recordXrun(uint64_t now = healthMonotonicMs()) {
        total_.fetch_add(1, std::memory_order_relaxed);
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
    uint64_t warningAt() const { return warningAt_.load(std::memory_order_acquire); }
    bool warningActive(uint64_t now = healthMonotonicMs()) const {
        const uint64_t warning = warningAt();
        const uint64_t grace = graceUntil_.load(std::memory_order_relaxed);
        return warning && warning >= grace && now >= warning && now - warning <= kWindowMs;
    }

private:
    std::atomic<uint64_t> graceUntil_, total_{0}, warningAt_{0};
    std::atomic<size_t> next_{0};
    std::atomic<uint64_t> recent_[kThreshold - 1]{};
};

inline AudioHealth audioHealth;

} // namespace audiox
