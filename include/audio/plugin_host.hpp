#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include "audio/plugin_ipc.hpp"

namespace audiox::plugin {
struct PackageMetadata;
class PluginHost {
public:
    enum class Status : uint32_t { Stopped, Starting, Ready, Fault };
    PluginHost() = default;
    ~PluginHost();
    PluginHost(const PluginHost&) = delete;
    PluginHost& operator=(const PluginHost&) = delete;
    // prepare(), poll(), stop() and destruction run on the control thread.
    // stop/destruction require the audio graph to have released this host.
    bool prepare(const char* module_path, const char* expected_id,
                 const AudioxParameterV1* expected_params, uint32_t param_count,
                 uint32_t sample_rate, uint32_t frames, uint32_t channels,
                 const char* worker_path = "/sbin/audiox-plugin-worker");
    bool prepare(const char* module_path, const PackageMetadata& metadata,
                 uint32_t sample_rate, uint32_t frames, uint32_t channels,
                 std::string& error, const char* worker_path = "/sbin/audiox-plugin-worker");
    void process(const float* const* ins, float* const* outs, uint32_t frames,
                 const float params[AUDIOX_PLUGIN_MAX_PARAMS]) noexcept;
    void reset() noexcept;
    bool hasPendingAudio(float threshold) const noexcept;
    Status status() const noexcept { return status_.load(std::memory_order_acquire); }
    const char* error() const noexcept { return error_; }
    void poll();
    void stop();
private:
    void fault(const char* error);
    void stopWorker();
    Mailbox* mailbox_ = nullptr;
    int pid_ = -1;
    int pid_fd_ = -1;
    uint32_t channels_ = 0, frames_ = 0, sequence_ = 0, in_flight_ = 0;
    uint32_t local_generation_ = 1, missed_ = 0;
    bool eligible_ = false, dry_valid_ = false;
    std::atomic<uint32_t> generation_{1}, pending_peak_{0}, pending_tail_{0};
    std::atomic<Status> status_{Status::Stopped};
    char error_[256]{};
    float dry_[kMaxFrames * kMaxChannels]{};
};
} // namespace audiox::plugin
