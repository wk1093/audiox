#include "audio/plugin_host.hpp"
#include "audio/plugin_package.hpp"
#include "audio/plugin_native.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
namespace audiox::plugin {
namespace {
std::atomic<bool> executionEnabled{false};
std::atomic<uint64_t> nextRevision{1};
using Clock = std::chrono::steady_clock;
bool transfer(int fd, void *data, size_t size, bool writing, Clock::time_point deadline) {
    auto *bytes = static_cast<unsigned char *>(data);
    while (size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) return false;
        pollfd descriptor{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        const int ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) return false;
        const ssize_t count = writing ? send(fd, bytes, size, MSG_NOSIGNAL) : recv(fd, bytes, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        bytes += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}
bool moduleFile(const char *path, std::string &error) {
    if (!path) { error = "missing plugin bitcode path"; return false; }
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) { error = "cannot open plugin module.bc; legacy plugins require rebuilding"; return false; }
    struct stat st{};
    unsigned char magic[4]{};
    const bool ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size >= 4 &&
        static_cast<uint64_t>(st.st_size) <= kMaxModuleBytes && read(fd, magic, 4) == 4;
    close(fd);
    if (!ok) { error = "invalid plugin bitcode file"; return false; }
    if (!std::memcmp(magic, "\177ELF", 4)) {
        error = "legacy ELF plugin: rebuild required as LLVM module.bc";
        return false;
    }
    if (std::memcmp(magic, "BC\300\336", 4) && std::memcmp(magic, "\336\300\027\013", 4)) {
        error = "invalid LLVM bitcode magic; rebuild required";
        return false;
    }
    return true;
}
bool compileImage(const char *path, const char *compiler, void *memory,
                  PluginNativeHeader &header, std::string &error) {
    using namespace audiox::graph;
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
        error = "cannot create plugin compiler socket";
        return false;
    }
    // Keep diagnostics separate from the binary image protocol.
    const int diagnostics = static_cast<int>(syscall(SYS_memfd_create, "audiox-plugin-diagnostics", 1U));
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    pid_t child = -1;
    if (!result) {
        result = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        if (!result) result = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        if (!result && diagnostics >= 0)
            result = posix_spawn_file_actions_adddup2(&actions, diagnostics, STDERR_FILENO);
        if (!result) result = posix_spawn_file_actions_addclose(&actions, sockets[0]);
        char *arguments[]{const_cast<char *>(compiler), const_cast<char *>("--plugin"),
                          const_cast<char *>(path), nullptr};
        if (!result) result = posix_spawn(&child, compiler, &actions, nullptr, arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
    }
    close(sockets[1]);
    CompileRequest request{};
    request.address = reinterpret_cast<uintptr_t>(memory);
#define IMPORT(fn) do { auto &entry = request.imports[request.importCount++]; \
    std::snprintf(entry.name, sizeof(entry.name), "%s", #fn); \
    entry.address = reinterpret_cast<uintptr_t>(&::fn); } while (false)
    IMPORT(malloc); IMPORT(calloc); IMPORT(realloc); IMPORT(free);
    IMPORT(memcpy); IMPORT(memmove); IMPORT(memset); IMPORT(memcmp);
    IMPORT(sinf); IMPORT(cosf); IMPORT(tanf); IMPORT(tanhf); IMPORT(powf);
    IMPORT(expf); IMPORT(exp2f); IMPORT(logf); IMPORT(log2f); IMPORT(log10f);
    IMPORT(sqrtf); IMPORT(fabsf); IMPORT(floorf); IMPORT(ceilf); IMPORT(fmodf);
    IMPORT(fminf); IMPORT(fmaxf); IMPORT(roundf); IMPORT(lroundf);
    IMPORT(sin); IMPORT(cos); IMPORT(tanh); IMPORT(pow); IMPORT(exp);
    IMPORT(log); IMPORT(sqrt); IMPORT(floor); IMPORT(ceil); IMPORT(fmod);
    IMPORT(abort);
#undef IMPORT
    const auto deadline = Clock::now() + std::chrono::seconds(10);
    bool ok = !result &&
        transfer(sockets[0], &request, sizeof(request), true, deadline) &&
        transfer(sockets[0], &header, sizeof(header), false, deadline);
    if (ok) {
        ok = header.magic == kPluginNativeMagic && header.size &&
            header.size <= kMaxNativeBytes && header.entryOffset < header.size &&
            header.sectionCount && header.sectionCount <= kMaxPluginSections;
        const long page = sysconf(_SC_PAGESIZE);
        bool entryExecutable = false;
        uint64_t previousEnd = 0;
        for (unsigned i = 0; ok && i < header.sectionCount; ++i) {
            const auto &section = header.sections[i];
            ok = page > 0 && section.offset % page == 0 &&
                section.offset >= previousEnd && section.offset <= header.size &&
                section.size <= header.size - section.offset && section.executable <= 1;
            previousEnd = static_cast<uint64_t>(section.offset) + section.size;
            if (section.executable && header.entryOffset >= section.offset &&
                header.entryOffset < previousEnd) entryExecutable = true;
        }
        ok = ok && entryExecutable;
        if (ok) ok = !mprotect(memory, kMaxNativeBytes, PROT_READ | PROT_WRITE) &&
            transfer(sockets[0], memory, header.size, false, deadline);
    }
    close(sockets[0]);
    bool reaped = false;
    int status{};
    while (ok && !reaped) {
        const pid_t waited = waitpid(child, &status, WNOHANG);
        if (waited == child) {
            reaped = true;
            ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
        } else if (waited < 0 && errno != EINTR) ok = false;
        else if (Clock::now() >= deadline) ok = false;
        else std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (child > 0 && !reaped) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    if (!ok) {
        error = result ? std::string("plugin compiler unavailable: ") + std::strerror(result) :
            "plugin compilation failed (invalid bitcode, ABI, unresolved symbol or timeout)";
        if (diagnostics >= 0) {
            char text[1024]{};
            const auto count = pread(diagnostics, text, sizeof(text) - 1, 0);
            if (count > 0) error = std::string(text, static_cast<size_t>(count));
        }
    }
    if (diagnostics >= 0) close(diagnostics);
    if (!ok) return false;
    __builtin___clear_cache(static_cast<char *>(memory), static_cast<char *>(memory) + header.size);
    if (mprotect(memory, kMaxNativeBytes, PROT_NONE)) {
        error = "cannot protect plugin image";
        return false;
    }
    const long page = sysconf(_SC_PAGESIZE);
    for (unsigned i = 0; i < header.sectionCount; ++i) {
        const auto &section = header.sections[i];
        if (!section.size) continue;
        const size_t bytes = (section.size + page - 1) / page * page;
        if (mprotect(static_cast<char *>(memory) + section.offset, bytes,
                     PROT_READ | (section.executable ? PROT_EXEC : PROT_WRITE))) {
            error = "cannot protect plugin section";
            return false;
        }
    }
    return true;
}
}

void setPluginExecutionEnabled(bool enabled) noexcept {
    executionEnabled.store(enabled, std::memory_order_release);
}
bool pluginExecutionEnabled() noexcept {
    return executionEnabled.load(std::memory_order_acquire);
}
PluginHost::~PluginHost() { stop(); }
void PluginHost::fault(const char *error) {
    std::snprintf(error_, sizeof(error_), "%s", error);
    status_.store(Status::Fault, std::memory_order_release);
}
bool PluginHost::prepare(const char *path, const PackageMetadata &metadata,
                         uint32_t rate, uint32_t frames, uint32_t channels,
                         std::string &error, const char *compiler) {
    AudioxParameterV1 parameters[AUDIOX_PLUGIN_MAX_PARAMS]{};
    if (metadata.paramCount > AUDIOX_PLUGIN_MAX_PARAMS) {
        error = "invalid plugin parameter count";
        fault(error.c_str());
        return false;
    }
    for (unsigned i = 0; i < metadata.paramCount; ++i) {
        const auto &p = metadata.params[i];
        parameters[i] = {p.name, p.label, p.min, p.max, p.defaultValue};
    }
    const bool ok = prepare(path, metadata.id, parameters, metadata.paramCount,
                            rate, frames, channels, compiler);
    error = ok ? "" : error_;
    return ok;
}
bool PluginHost::prepare(const char *path, const char *id,
                         const AudioxParameterV1 *parameters, uint32_t count,
                         uint32_t rate, uint32_t frames, uint32_t channels,
                         const char *compiler) {
    stop();
    revision_ = nextRevision.fetch_add(1, std::memory_order_relaxed);
    status_.store(Status::Starting, std::memory_order_release);
    std::string error;
    if (!id || !validId(id) || count > AUDIOX_PLUGIN_MAX_PARAMS || (count && !parameters) ||
        !rate || !frames || frames > 8192 || !channels || channels > 2 || !compiler) {
        fault("invalid plugin preparation configuration");
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto &p = parameters[i];
        if (!p.name || !p.label || !std::isfinite(p.min) || !std::isfinite(p.max) ||
            !std::isfinite(p.default_value) || p.min > p.max ||
            p.default_value < p.min || p.default_value > p.max) {
            fault("invalid plugin parameter metadata");
            return false;
        }
    }
    if (!moduleFile(path, error)) { fault(error.c_str()); return false; }
    channels_ = channels;
    frames_ = frames;
    if (!pluginExecutionEnabled()) {
        status_.store(Status::Ready, std::memory_order_release);
        return true;
    }
    memory_ = mmap(nullptr, audiox::graph::kMaxNativeBytes, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory_ == MAP_FAILED) {
        memory_ = nullptr;
        fault("cannot allocate plugin native image");
        return false;
    }
    PluginNativeHeader header{};
    if (!compileImage(path, compiler, memory_, header, error)) {
        stop();
        fault(error.c_str());
        return false;
    }
    const auto entry = reinterpret_cast<AudioxPluginEntryV1>(
        static_cast<char *>(memory_) + header.entryOffset);
    api_ = entry();
    if (!api_ || api_->abi_version != AUDIOX_PLUGIN_ABI_VERSION ||
        api_->struct_size < sizeof(AudioxPluginV1) || !api_->id ||
        std::strcmp(api_->id, id) || api_->param_count != count ||
        (count && !api_->params) || !api_->create || !api_->destroy ||
        !api_->reset || !api_->process || !api_->tail) {
        api_ = nullptr;
        stop();
        fault("plugin ABI or metadata mismatch");
        return false;
    }
    for (unsigned i = 0; i < count; ++i) {
        const auto &a = api_->params[i], &b = parameters[i];
        if (!a.name || !a.label || std::strcmp(a.name, b.name) ||
            std::strcmp(a.label, b.label) || a.min != b.min || a.max != b.max ||
            a.default_value != b.default_value) {
            api_ = nullptr;
            stop();
            fault("plugin ABI parameter metadata mismatch");
            return false;
        }
    }
    for (unsigned c = 0; c < channels_; ++c) {
        instances_[c] = api_->create(rate, frames);
        if (!instances_[c]) {
            stop();
            fault("plugin create failed");
            return false;
        }
        api_->reset(instances_[c]);
    }
    status_.store(Status::Ready, std::memory_order_release);
    return true;
}
void PluginHost::process(const float *const *ins, float *const *outs, uint32_t frames,
                         const float params[AUDIOX_PLUGIN_MAX_PARAMS]) noexcept {
    if (!ins || !outs) return;
    if (!api_ || !pluginExecutionEnabled() || status() != Status::Ready || frames > frames_) {
        for (unsigned c = 0; c < channels_; ++c)
            if (ins[c] && outs[c]) std::memmove(outs[c], ins[c], frames * sizeof(float));
        return;
    }
    if (resetRequested_.exchange(false, std::memory_order_acq_rel))
        for (unsigned c = 0; c < channels_; ++c) api_->reset(instances_[c]);
    for (unsigned c = 0; c < channels_; ++c) {
        api_->process(instances_[c], ins[c], outs[c], frames, params);
        for (unsigned i = 0; i < frames; ++i)
            if (!std::isfinite(outs[c][i])) {
                std::memmove(outs[c], ins[c], frames * sizeof(float));
                fault("plugin produced non-finite output");
                break;
            }
    }
}
void PluginHost::reset() noexcept {
    resetRequested_.store(true, std::memory_order_release);
}
bool PluginHost::hasPendingAudio(float threshold) const noexcept {
    if (!api_ || !api_->tail || !pluginExecutionEnabled() ||
        resetRequested_.load(std::memory_order_acquire) || status() != Status::Ready)
        return false;
    for (unsigned c = 0; c < channels_; ++c)
        if (api_->tail(instances_[c], threshold)) return true;
    return false;
}
void PluginHost::poll() {}
void PluginHost::stop() {
    if (api_)
        for (void *&instance : instances_) {
            if (instance) api_->destroy(instance);
            instance = nullptr;
        }
    api_ = nullptr;
    if (memory_) munmap(memory_, audiox::graph::kMaxNativeBytes);
    memory_ = nullptr;
    channels_ = frames_ = 0;
    resetRequested_.store(false, std::memory_order_relaxed);
    error_[0] = 0;
    status_.store(Status::Stopped, std::memory_order_release);
}
} // namespace audiox::plugin
