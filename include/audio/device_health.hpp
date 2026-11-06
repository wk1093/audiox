#pragma once

#include <atomic>
#include <cstdint>

namespace audiox {

struct DeviceStreamHealth {
    char id[64]{};
    uint32_t rate = 0;
    uint32_t period = 0;
    uint32_t buffer = 0;
    uint64_t xruns = 0;
    bool capture = false;
    bool active = false;
    bool mmap = false;
};

struct DeviceHealthSnapshot {
    static constexpr unsigned kMaxStreams = 64;
    uint64_t sampledAt = 0;
    unsigned count = 0;
    DeviceStreamHealth streams[kMaxStreams]{};
};

// Single audio-thread publisher and single main-loop consumer exchange ownership.
class DeviceHealthMailbox {
public:
    void publish(const DeviceHealthSnapshot &snapshot) {
        slots_[back_] = snapshot;
        back_ = middle_.exchange(back_ | 4U, std::memory_order_acq_rel) & 3U;
    }
    const DeviceHealthSnapshot &read() {
        if (middle_.load(std::memory_order_acquire) & 4U)
            front_ = middle_.exchange(front_, std::memory_order_acq_rel) & 3U;
        return slots_[front_];
    }
private:
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    DeviceHealthSnapshot slots_[3]{};
    std::atomic<unsigned> middle_{1};
    unsigned back_ = 2;
    unsigned front_ = 0;
};

}
