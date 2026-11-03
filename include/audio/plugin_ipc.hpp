#pragma once

#include <atomic>
#include <cstdint>
#include "audio/plugin_api.h"

namespace audiox::plugin {
constexpr uint32_t kMaxFrames = 1024;
constexpr uint32_t kMaxChannels = 16;
constexpr uint32_t kMaxMissedCycles = 32;
constexpr uint32_t kIpcMagic = 0x41585031;
enum class WorkerState : uint32_t { Starting, Ready, Fault };
struct ParameterMetadata {
    char name[64];
    char label[128];
    float min, max, default_value;
};
// Exactly one producer/consumer transaction may own the audio arrays at a time.
// Release/acquire sequence publication transfers ownership, not a shared lock.
struct Mailbox {
    uint32_t magic;
    uint32_t sample_rate, max_frames, channels, param_count;
    char id[128];
    ParameterMetadata expected[AUDIOX_PLUGIN_MAX_PARAMS];
    std::atomic<uint32_t> state{static_cast<uint32_t>(WorkerState::Starting)};
    std::atomic<uint32_t> request{0}, response{0}, generation{1};
    std::atomic<uint32_t> tail_threshold{0};
    uint32_t request_generation, response_generation, frames, tail;
    float params[AUDIOX_PLUGIN_MAX_PARAMS];
    char error[256];
    alignas(64) float input[kMaxFrames * kMaxChannels];
    alignas(64) float output[kMaxFrames * kMaxChannels];
};
static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "The DSP mailbox requires lock-free 32-bit atomics");
} // namespace audiox::plugin
