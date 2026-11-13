#pragma once

#include <alsa/asoundlib.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

namespace audiox {

struct PcmOpenRequest {
    uint32_t ticket{};
    bool capture{};
    uint8_t channels{};
    uint32_t bufferLevel{};
    uint32_t targetPeriod{};
    char path[64]{};
};

struct PcmOpenResult {
    uint32_t ticket{};
    bool capture{};
    int err{};
    snd_pcm_t *pcm{};
    snd_pcm_format_t format{SND_PCM_FORMAT_UNKNOWN};
    uint8_t channels{};
    uint32_t sampleRate{};
    uint32_t periodFrames{};
    uint32_t bufferFrames{};
    uint32_t bufferLevel{};
    bool mmap{};
    char path[64]{};
};

// Remembers PCM devices that failed to open so the audio thread retries them
// with exponential backoff instead of on a fixed short cadence. Audio-thread only.
class PcmRetryRegistry {
  public:
    static constexpr unsigned kCapacity = 16;
    static constexpr uint64_t kFirstDelayMs = 1000;
    static constexpr uint64_t kMaxDelayMs = 60000;

    bool shouldAttempt(const char *path, bool capture, uint64_t nowMs) const {
        const Entry *entry = find(path, capture);
        return !entry || nowMs >= entry->nextAttemptMs;
    }

    // Returns the delay until the next attempt is allowed.
    uint64_t recordFailure(const char *path, bool capture, uint64_t nowMs) {
        Entry *entry = find(path, capture);
        if (!entry) entry = allocate(path, capture);
        if (entry->failures < 32) ++entry->failures;
        uint64_t delay = kFirstDelayMs;
        for (uint32_t i = 1; i < entry->failures && delay < kMaxDelayMs; ++i) delay *= 2;
        if (delay > kMaxDelayMs) delay = kMaxDelayMs;
        entry->nextAttemptMs = nowMs + delay;
        entry->lastUsedMs = nowMs;
        return delay;
    }

    uint32_t failures(const char *path, bool capture) const {
        const Entry *entry = find(path, capture);
        return entry ? entry->failures : 0;
    }

    void recordSuccess(const char *path, bool capture) {
        if (Entry *entry = find(path, capture)) *entry = Entry{};
    }

    void clear() {
        for (Entry &entry : entries_) entry = Entry{};
    }

  private:
    struct Entry {
        bool used{};
        bool capture{};
        uint32_t failures{};
        uint64_t nextAttemptMs{};
        uint64_t lastUsedMs{};
        char path[64]{};
    };
    Entry entries_[kCapacity]{};

    const Entry *find(const char *path, bool capture) const {
        for (const Entry &entry : entries_)
            if (entry.used && entry.capture == capture && strcmp(entry.path, path) == 0) return &entry;
        return nullptr;
    }
    Entry *find(const char *path, bool capture) {
        return const_cast<Entry *>(static_cast<const PcmRetryRegistry *>(this)->find(path, capture));
    }
    Entry *allocate(const char *path, bool capture) {
        Entry *slot = &entries_[0];
        for (Entry &entry : entries_) {
            if (!entry.used) { slot = &entry; break; }
            if (entry.lastUsedMs < slot->lastUsedMs) slot = &entry;
        }
        *slot = Entry{};
        slot->used = true;
        slot->capture = capture;
        snprintf(slot->path, sizeof(slot->path), "%s", path);
        return slot;
    }
};

// Runs slow PCM open/hw_params negotiation on a normal-priority helper thread so
// a device that is slow or refuses to open never stalls the realtime audio
// thread. Only the audio thread calls submit/poll; the helper owns a slot while
// it is Requested.
class PcmOpener {
  public:
    using OpenFn = void (*)(const PcmOpenRequest &, PcmOpenResult *);
    using DiscardFn = void (*)(PcmOpenResult &);
    static constexpr unsigned kSlots = 8;

    PcmOpener(OpenFn open, DiscardFn discard) : open_(open), discard_(discard) {
        if (sem_init(&wake_, 0, 0) != 0) return;
        wakeInitialized_ = true;
        running_.store(true, std::memory_order_release);
        if (pthread_create(&thread_, nullptr, threadMain, this) != 0) {
            running_.store(false, std::memory_order_release);
            return;
        }
        started_ = true;
    }

    ~PcmOpener() {
        running_.store(false, std::memory_order_release);
        if (started_) {
            (void)sem_post(&wake_);
            pthread_join(thread_, nullptr);
        }
        if (wakeInitialized_) sem_destroy(&wake_);
        for (Slot &slot : slots_) {
            if (slot.state.load(std::memory_order_acquire) == kDone && discard_) discard_(slot.result);
        }
    }

    PcmOpener(const PcmOpener &) = delete;
    PcmOpener &operator=(const PcmOpener &) = delete;

    bool started() const { return started_; }

    // Returns the request ticket, or 0 when no slot is free or the helper is not running.
    uint32_t submit(const PcmOpenRequest &request) {
        if (!started_) return 0;
        for (Slot &slot : slots_) {
            if (slot.state.load(std::memory_order_acquire) != kIdle) continue;
            if (++nextTicket_ == 0) ++nextTicket_;
            slot.request = request;
            slot.request.ticket = nextTicket_;
            slot.state.store(kRequested, std::memory_order_release);
            (void)sem_post(&wake_);
            return nextTicket_;
        }
        return 0;
    }

    bool poll(PcmOpenResult *out) {
        for (Slot &slot : slots_) {
            if (slot.state.load(std::memory_order_acquire) != kDone) continue;
            *out = slot.result;
            slot.state.store(kIdle, std::memory_order_release);
            return true;
        }
        return false;
    }

  private:
    enum : unsigned { kIdle = 0, kRequested = 1, kDone = 2 };
    struct Slot {
        std::atomic<unsigned> state{kIdle};
        PcmOpenRequest request{};
        PcmOpenResult result{};
    };

    Slot slots_[kSlots]{};
    OpenFn open_;
    DiscardFn discard_;
    uint32_t nextTicket_{};
    std::atomic<bool> running_{false};
    sem_t wake_{};
    bool wakeInitialized_{};
    bool started_{};
    pthread_t thread_{};

    static void *threadMain(void *arg) {
        PcmOpener &self = *static_cast<PcmOpener *>(arg);
#ifdef __linux__
        pthread_setname_np(pthread_self(), "audiox-pcmopen");
#endif
        while (self.running_.load(std::memory_order_acquire)) {
            bool worked = false;
            for (Slot &slot : self.slots_) {
                if (slot.state.load(std::memory_order_acquire) != kRequested) continue;
                PcmOpenResult result{};
                result.ticket = slot.request.ticket;
                result.capture = slot.request.capture;
                snprintf(result.path, sizeof(result.path), "%s", slot.request.path);
                self.open_(slot.request, &result);
                slot.result = result;
                slot.state.store(kDone, std::memory_order_release);
                worked = true;
            }
            if (worked) continue;
            timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_sec += 1;
            while (sem_timedwait(&self.wake_, &deadline) != 0 && errno == EINTR) {}
        }
        return nullptr;
    }
};

} // namespace audiox
