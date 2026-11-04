#include "audio/plugin_host.hpp"
#include "audio/plugin_package.hpp"

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <limits.h>
#include <new>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char** environ;
namespace audiox::plugin {
namespace {
bool copyText(char* dst, size_t capacity, const char* src) {
    if (!src) return false;
    const size_t length = ::strnlen(src, capacity);
    if (length >= capacity) return false;
    std::memcpy(dst, src, length + 1);
    return true;
}
}
PluginHost::~PluginHost() { stop(); }
bool PluginHost::prepare(const char* path, const PackageMetadata& metadata,
                         uint32_t rate, uint32_t frames, uint32_t channels,
                         std::string& error, const char* worker) {
    AudioxParameterV1 specs[AUDIOX_PLUGIN_MAX_PARAMS]{};
    if (metadata.api != AUDIOX_PLUGIN_ABI_VERSION ||
        metadata.paramCount > AUDIOX_PLUGIN_MAX_PARAMS ||
        ::strnlen(metadata.id, sizeof(metadata.id)) == sizeof(metadata.id)) {
        stop(); error = "Invalid plugin metadata"; fault(error.c_str()); return false;
    }
    for (uint32_t i = 0; i < metadata.paramCount; ++i) {
        const auto& p = metadata.params[i];
        if (::strnlen(p.name, sizeof(p.name)) == sizeof(p.name) ||
            ::strnlen(p.label, sizeof(p.label)) == sizeof(p.label)) {
            stop(); error = "Invalid plugin parameter metadata"; fault(error.c_str()); return false;
        }
        specs[i] = {p.name, p.label, p.min, p.max, p.defaultValue};
    }
    const bool ok = prepare(path, metadata.id, specs, metadata.paramCount,
                            rate, frames, channels, worker);
    if (!ok) error = this->error(); else error.clear();
    return ok;
}
void PluginHost::fault(const char* text) {
    std::snprintf(error_, sizeof(error_), "%s", text);
    status_.store(Status::Fault, std::memory_order_release);
}
bool PluginHost::prepare(const char* path, const char* id, const AudioxParameterV1* specs,
                         uint32_t count, uint32_t rate, uint32_t frames,
                         uint32_t channels, const char* worker) {
    stop();
    error_[0] = '\0';
    if (!path || !worker || !rate || !frames || frames > kMaxFrames ||
        !channels || channels > kMaxChannels || count > AUDIOX_PLUGIN_MAX_PARAMS ||
        (count && !specs)) { fault("Invalid plugin host configuration"); return false; }
    char module_path[PATH_MAX];
    if (!realpath(path, module_path)) { fault("Cannot resolve plugin module path"); return false; }
    int fd = static_cast<int>(syscall(SYS_memfd_create, "audiox-plugin-ipc", 0));
    if (fd < 0) { fault("Cannot allocate plugin IPC"); return false; }
    if (fd < 3) {
        const int inherited = fcntl(fd, F_DUPFD, 3);
        close(fd);
        fd = inherited;
        if (fd < 0) { fault("Cannot reserve plugin IPC descriptor"); return false; }
    }
    if (ftruncate(fd, sizeof(Mailbox)) != 0) {
        close(fd); fault("Cannot size plugin IPC"); return false;
    }
    void* memory = mmap(nullptr, sizeof(Mailbox), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) { close(fd); fault("Cannot map plugin IPC"); return false; }
    mailbox_ = new(memory) Mailbox{};
    mailbox_->magic = kIpcMagic;
    mailbox_->sample_rate = rate; mailbox_->max_frames = frames;
    mailbox_->channels = channels; mailbox_->param_count = count;
    bool valid = copyText(mailbox_->id, sizeof(mailbox_->id), id);
    for (uint32_t i = 0; i < count; ++i) {
        auto& dst = mailbox_->expected[i];
        valid = copyText(dst.name, sizeof(dst.name), specs[i].name) && valid;
        valid = copyText(dst.label, sizeof(dst.label), specs[i].label) && valid;
        dst.min = specs[i].min; dst.max = specs[i].max; dst.default_value = specs[i].default_value;
        valid = valid && std::isfinite(dst.min) && std::isfinite(dst.max) &&
                std::isfinite(dst.default_value) && dst.min <= dst.max &&
                dst.default_value >= dst.min && dst.default_value <= dst.max;
    }
    if (!valid) { close(fd); stop(); fault("Invalid expected plugin metadata"); return false; }
    char fd_text[24];
    std::snprintf(fd_text, sizeof(fd_text), "%d", fd);
    char* argv[] = {const_cast<char*>(worker), fd_text, module_path, nullptr};
    pid_t child;
    status_.store(Status::Starting, std::memory_order_release);
    const int result = posix_spawn(&child, worker, nullptr, nullptr, argv, environ);
    close(fd);
    if (result) { stop(); fault("Cannot launch plugin worker"); return false; }
    pid_ = child; frames_ = frames; channels_ = channels;
    pid_fd_ = static_cast<int>(syscall(SYS_pidfd_open, pid_, 0u));
    if (pid_fd_ < 0) { stop(); fault("Cannot obtain plugin worker process handle"); return false; }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1750);
    while (std::chrono::steady_clock::now() < deadline) {
        const auto state = static_cast<WorkerState>(mailbox_->state.load(std::memory_order_acquire));
        if (state == WorkerState::Ready) {
            status_.store(Status::Ready, std::memory_order_release);
            return true;
        }
        if (state == WorkerState::Fault) break;
        int status;
        const pid_t waited = waitpid(pid_, &status, WNOHANG);
        if (waited == pid_ || (waited < 0 && errno == ECHILD)) { pid_ = -1; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    char message[256];
    std::memcpy(message, mailbox_->error, sizeof(message));
    message[sizeof(message) - 1] = '\0';
    stop();
    fault(message[0] ? message : "Plugin worker failed or startup timed out");
    return false;
}
void PluginHost::process(const float* const* ins, float* const* outs, uint32_t frames,
                         const float params[AUDIOX_PLUGIN_MAX_PARAMS]) noexcept {
    if (!ins || !outs || !frames || frames > kMaxFrames) return;
    const uint32_t generation = generation_.load(std::memory_order_acquire);
    if (generation != local_generation_) {
        local_generation_ = generation;
        dry_valid_ = false; eligible_ = false;
        pending_tail_.store(0, std::memory_order_relaxed);
        pending_peak_.store(0, std::memory_order_relaxed);
    }
    bool ready = status() == Status::Ready && frames == frames_ && mailbox_;
    bool wet = false;
    if (ready) {
        if (mailbox_->state.load(std::memory_order_acquire) !=
            static_cast<uint32_t>(WorkerState::Ready)) {
            status_.store(Status::Fault, std::memory_order_release); ready = false;
        } else if (in_flight_) {
            const uint32_t response = mailbox_->response.load(std::memory_order_acquire);
            if (response == in_flight_) {
                wet = eligible_ && mailbox_->response_generation == generation;
                if (wet) pending_tail_.store(mailbox_->tail != 0, std::memory_order_relaxed);
                in_flight_ = 0; missed_ = 0;
            } else {
                eligible_ = false;
                if (++missed_ >= kMaxMissedCycles) {
                    status_.store(Status::Fault, std::memory_order_release); ready = false;
                }
            }
        }
    }
    float peak = 0;
    for (uint32_t c = 0; c < channels_; ++c) {
        for (uint32_t f = 0; f < frames; ++f) {
            const size_t index = c * frames_ + f;
            const float input = std::isfinite(ins[c][f]) ? ins[c][f] : 0.0f;
            float sample = wet ? mailbox_->output[index] :
                (dry_valid_ && frames == frames_ ? dry_[index] : 0.0f);
            if (!std::isfinite(sample)) sample = 0.0f;
            outs[c][f] = sample;
            dry_[index] = input;
            peak = std::max(peak, std::fabs(input));
        }
    }
    dry_valid_ = frames == frames_;
    uint32_t peak_bits;
    std::memcpy(&peak_bits, &peak, sizeof(peak_bits));
    pending_peak_.store(peak_bits, std::memory_order_relaxed);
    if (ready && !in_flight_) {
        std::memcpy(mailbox_->input, dry_, channels_ * frames_ * sizeof(float));
        for (uint32_t p = 0; p < AUDIOX_PLUGIN_MAX_PARAMS; ++p)
            mailbox_->params[p] = params && std::isfinite(params[p]) ? params[p] : 0.0f;
        mailbox_->frames = frames;
        mailbox_->request_generation = generation;
        if (++sequence_ == 0) ++sequence_;
        in_flight_ = sequence_; eligible_ = true;
        mailbox_->request.store(sequence_, std::memory_order_release);
    }
}
void PluginHost::reset() noexcept {
    const uint32_t generation = generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    pending_peak_.store(0, std::memory_order_relaxed);
    pending_tail_.store(0, std::memory_order_relaxed);
    if (mailbox_) mailbox_->generation.store(generation, std::memory_order_release);
}
bool PluginHost::hasPendingAudio(float threshold) const noexcept {
    if (status() != Status::Ready) return false;
    const float requested = std::isfinite(threshold) ? std::max(0.0f, threshold) : 0.0f;
    uint32_t threshold_bits;
    std::memcpy(&threshold_bits, &requested, sizeof(threshold_bits));
    mailbox_->tail_threshold.store(threshold_bits, std::memory_order_relaxed);
    float peak;
    uint32_t bits = pending_peak_.load(std::memory_order_relaxed);
    std::memcpy(&peak, &bits, sizeof(peak));
    return pending_tail_.load(std::memory_order_relaxed) ||
           peak > requested;
}
void PluginHost::poll() {
    if (pid_ <= 0) return;
    int status;
    const pid_t waited = waitpid(pid_, &status, WNOHANG);
    if (waited == pid_ || (waited < 0 && errno == ECHILD)) {
        pid_ = -1;
        fault("Plugin worker exited");
    } else if (this->status() == Status::Fault && !error_[0]) {
        char text[256];
        std::memcpy(text, mailbox_->error, sizeof(text));
        text[sizeof(text) - 1] = '\0';
        fault(text[0] ? text : "Plugin worker missed its processing deadline");
    }
    if (this->status() == Status::Fault && pid_ > 0)
        (void)syscall(SYS_pidfd_send_signal, pid_fd_, SIGKILL, nullptr, 0u);
    else if (pid_ <= 0 && pid_fd_ >= 0) {
        close(pid_fd_);
        pid_fd_ = -1;
    }
}
void PluginHost::stopWorker() {
    if (pid_ > 0) {
        if (pid_fd_ >= 0) syscall(SYS_pidfd_send_signal, pid_fd_, SIGTERM, nullptr, 0u);
        else kill(pid_, SIGTERM);
        bool reaped = false;
        for (unsigned i = 0; i < 50; ++i) {
            int status;
            const pid_t waited = waitpid(pid_, &status, WNOHANG);
            if (waited == pid_ || (waited < 0 && errno == ECHILD)) { reaped = true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (!reaped) {
            if (pid_fd_ >= 0) syscall(SYS_pidfd_send_signal, pid_fd_, SIGKILL, nullptr, 0u);
            else kill(pid_, SIGKILL);
            while (waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {}
        }
        pid_ = -1;
    }
    if (pid_fd_ >= 0) { close(pid_fd_); pid_fd_ = -1; }
}
void PluginHost::stop() {
    status_.store(Status::Stopped, std::memory_order_release);
    stopWorker();
    if (mailbox_) { munmap(mailbox_, sizeof(Mailbox)); mailbox_ = nullptr; }
    channels_ = frames_ = sequence_ = in_flight_ = missed_ = 0;
    local_generation_ = 1; generation_.store(1, std::memory_order_relaxed);
    dry_valid_ = eligible_ = false;
    pending_peak_.store(0, std::memory_order_relaxed);
    pending_tail_.store(0, std::memory_order_relaxed);
}
} // namespace audiox::plugin
