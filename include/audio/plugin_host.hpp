#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include "audio/plugin_api.h"

namespace audiox::plugin {
// Only the exec'd audio worker opts in. The application never executes plugins.
void setPluginExecutionEnabled(bool enabled) noexcept;
bool pluginExecutionEnabled() noexcept;
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
    // process(), reset(), hasPendingAudio() are called by the audio thread.
    bool prepare(const char* module_path, const char* expected_id,
                 const AudioxParameterV1* expected_params, uint32_t param_count,
                 uint32_t sample_rate, uint32_t frames, uint32_t channels,
                 const char* compiler_path = "/sbin/audiox-graph-compiler");
    bool prepare(const char* module_path, const PackageMetadata& metadata,
                 uint32_t sample_rate, uint32_t frames, uint32_t channels,
                 std::string& error, const char* compiler_path = "/sbin/audiox-graph-compiler");
    void process(const float* const* ins, float* const* outs, uint32_t frames,
                 const float params[AUDIOX_PLUGIN_MAX_PARAMS]) noexcept;
    void reset() noexcept;
    bool hasPendingAudio(float threshold) const noexcept;
    Status status() const noexcept { return status_.load(std::memory_order_acquire); }
    uint64_t revision() const noexcept { return revision_; }
    const char* error() const noexcept { return error_; }
    void poll();
    void stop();
private:
    void fault(const char* error);
    void* memory_ = nullptr;
    const AudioxPluginV1* api_ = nullptr;
    void* instances_[2]{};
    uint32_t channels_ = 0, frames_ = 0;
    uint64_t revision_ = 0;
    std::atomic<bool> resetRequested_{false};
    std::atomic<Status> status_{Status::Stopped};
    char error_[256]{};
};
} // namespace audiox::plugin
