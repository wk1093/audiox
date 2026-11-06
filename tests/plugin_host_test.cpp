#include "audio/plugin_api.h"
#ifdef AUDIOX_PLUGIN_TEST_FIXTURE
#include <cstdlib>
#include <cstring>
#include <array>
#include <cstdio>
#include <fcntl.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
namespace {
struct State { float previous[1024]; uint32_t frames; };
constexpr uint32_t gainIndex = AUDIOX_PLUGIN_TEST_FIXTURE == 12 ? AUDIOX_PLUGIN_MAX_PARAMS - 1 : 0;
#if AUDIOX_PLUGIN_TEST_FIXTURE == 12
char parameterNames[AUDIOX_PLUGIN_MAX_PARAMS][24];
const auto parameters = [] {
    std::array<AudioxParameterV1, AUDIOX_PLUGIN_MAX_PARAMS> result{};
    for (unsigned i = 0; i < result.size(); ++i) {
        std::snprintf(parameterNames[i], sizeof(parameterNames[i]), "p%u", i);
        result[i] = {parameterNames[i], "Parameter", -2, 2, 1};
    }
    return result;
}();
#else
const AudioxParameterV1 parameters[] = {{"gain", "Gain", -2, 2, 1}};
#endif
[[maybe_unused]] bool forbidden_read = false;
__attribute__((constructor)) void constructor() {
#if AUDIOX_PLUGIN_TEST_FIXTURE == 1
    std::abort();
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 4
    (void)open("plugin-constructor-escape", O_CREAT | O_WRONLY, 0600);
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 7 || AUDIOX_PLUGIN_TEST_FIXTURE == 11
    const char* paths[] = {"/etc/passwd", "../../etc/passwd", "/proc/self/root/etc/passwd",
                           "/dev/null", "/tmp/audiox-plugin-secret"};
    for (const char* path : paths) {
        const int fd = open(path, O_RDONLY);
        forbidden_read |= fd >= 0;
        if (fd >= 0) close(fd);
    }
#if AUDIOX_PLUGIN_TEST_FIXTURE == 11
    const int fd = open("/module.so", O_RDONLY);
    forbidden_read |= fd < 0;
    if (fd >= 0) close(fd);
#endif
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 9
    for (;;) asm volatile("" ::: "memory");
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 10
    signal(SIGTERM, SIG_IGN);
#endif
}
void* create(uint32_t, uint32_t) {
#if AUDIOX_PLUGIN_TEST_FIXTURE == 8
    return std::calloc(1, 128 * 1024 * 1024);
#else
    return forbidden_read ? nullptr : std::calloc(1, sizeof(State));
#endif
}
void destroy(void* instance) { std::free(instance); }
void reset(void* instance) { std::memset(instance, 0, sizeof(State)); }
void process(void* instance, const float* input, float* output, uint32_t frames, const float* params) {
#if AUDIOX_PLUGIN_TEST_FIXTURE == 2
    std::abort();
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 3 || AUDIOX_PLUGIN_TEST_FIXTURE == 10
    for (;;) asm volatile("" ::: "memory");
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 5
    (void)socket(AF_INET, SOCK_STREAM, 0);
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 6
    const timespec pause{0, 20000000};
    nanosleep(&pause, nullptr);
#endif
    auto* state = static_cast<State*>(instance);
    for (uint32_t i = 0; i < frames; ++i)
        output[i] = input[i] * params[gainIndex] + state->previous[i] * 0.25f;
    std::memcpy(state->previous, input, frames * sizeof(float));
    state->frames = frames;
}
int tail(void* instance, float threshold) {
    auto* state = static_cast<State*>(instance);
    for (uint32_t i = 0; i < state->frames; ++i)
        if (state->previous[i] > threshold || state->previous[i] < -threshold) return 1;
    return 0;
}
const AudioxPluginV1 descriptor = {
#if AUDIOX_PLUGIN_TEST_FIXTURE == 12
    AUDIOX_PLUGIN_ABI_VERSION, sizeof(AudioxPluginV1), "plugin_fixture", AUDIOX_PLUGIN_MAX_PARAMS,
    parameters.data(), create, destroy, reset, process, tail
#else
    AUDIOX_PLUGIN_ABI_VERSION, sizeof(AudioxPluginV1), "plugin_fixture", 1,
    parameters, create, destroy, reset, process, tail
#endif
};
}
extern "C" const AudioxPluginV1* audiox_plugin_v1() { return &descriptor; }
#else
#include "audio/plugin_host.hpp"
#include "audio/plugin_package.hpp"
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#ifdef AUDIOX_PLUGIN_NAMESPACE_TEST
#include <cerrno>
#include <fcntl.h>
#include <linux/landlock.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#endif

using audiox::plugin::PluginHost;
namespace {
constexpr uint32_t frames = 64;
const AudioxParameterV1 specs[] = {{"gain", "Gain", -2, 2, 1}};
struct Audio {
    float in[2][frames]{}, out[2][frames]{};
    const float* inputs[2]{in[0], in[1]};
    float* outputs[2]{out[0], out[1]};
    float params[AUDIOX_PLUGIN_MAX_PARAMS]{2};
    void fill(float a, float b) {
        for (uint32_t i = 0; i < frames; ++i) { in[0][i] = a; in[1][i] = b; }
    }
    void check(float a, float b) {
        for (uint32_t i = 0; i < frames; ++i) {
            assert(std::fabs(out[0][i] - a) < 0.00001f);
            assert(std::fabs(out[1][i] - b) < 0.00001f);
        }
    }
    void run(PluginHost& host) { host.process(inputs, outputs, frames, params); }
};
void settle() { std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
bool prepare(PluginHost& host, const char* worker, const std::string& module) {
    const bool ok = host.prepare(module.c_str(), "plugin_fixture", specs, 1,
                                 48000, frames, 2, worker);
    if (!ok) std::fprintf(stderr, "%s: %s\n", module.c_str(), host.error());
    return ok;
}
}
int main(int argc, char** argv) {
#ifdef AUDIOX_PLUGIN_NAMESPACE_TEST
    assert(getpid() == 1 && geteuid() == 0);
    assert(chroot("/runtime") == 0 && chdir("/") == 0);
    assert(mount("devtmpfs", "/dev", "devtmpfs", 0, nullptr) == 0);
    const int console = open("/dev/console", O_RDWR);
    assert(console >= 0);
    for (int fd = 0; fd < 3; ++fd) assert(dup2(console, fd) == fd);
    if (console > 2) close(console);
    assert(mount("proc", "/proc", "proc", 0, nullptr) == 0);
    assert(syscall(SYS_landlock_create_ruleset, nullptr, 0,
                   LANDLOCK_CREATE_RULESET_VERSION) == -1);
    assert(errno == ENOSYS || errno == EOPNOTSUPP);
    struct stat original_namespace{};
    assert(stat("/proc/self/ns/mnt", &original_namespace) == 0);
    assert(rmdir("/tmp") == 0);
    assert(access("/tmp", F_OK) == -1 && errno == ENOENT);
    char program[] = "/init", worker[] = "/sbin/audiox-plugin-worker", fixtures[] = "/plugins";
    char* arguments[] = {program, worker, fixtures, nullptr};
    argc = 3;
    argv = arguments;
#endif
    assert(argc == 3 && "usage: plugin_host_test WORKER FIXTURE_DIRECTORY");
    const auto path = [&](unsigned kind) {
        return std::string(argv[2]) + "/plugin-fixture-" + std::to_string(kind) + ".so";
    };
    auto host = std::make_unique<PluginHost>();
    assert(prepare(*host, argv[1], path(0)));
#ifdef AUDIOX_PLUGIN_NAMESPACE_TEST
    const int secret = open("/tmp/audiox-plugin-secret", O_CREAT | O_EXCL | O_WRONLY, 0644);
    assert(secret >= 0);
    assert(write(secret, "private", 7) == 7);
    close(secret);
#endif
    Audio audio;
    audio.fill(0.1f, 0.3f); audio.run(*host); audio.check(0, 0);
    settle(); audio.fill(0.2f, 0.4f); audio.run(*host); audio.check(0.2f, 0.6f);
    settle(); audio.fill(0, 0); audio.run(*host); audio.check(0.425f, 0.875f);
    assert(host->hasPendingAudio(0.01f));
    settle(); audio.run(*host); audio.check(0.05f, 0.1f);
    settle(); audio.run(*host); audio.check(0, 0);
    assert(!host->hasPendingAudio(0.01f));
    auto second = std::make_unique<PluginHost>();
    assert(prepare(*second, argv[1], path(0)));
    Audio independent;
    independent.params[0] = 0.5f;
    independent.fill(0.2f, -0.2f); independent.run(*second);
    audio.fill(0.1f, 0.3f); audio.run(*host);
    settle(); independent.run(*second); independent.check(0.1f, -0.1f);
    audio.run(*host); audio.check(0.2f, 0.6f);
    {
        auto softclip = std::make_unique<PluginHost>();
        const AudioxParameterV1 drive[] = {{"drive", "Drive", 0.1f, 10.0f, 1.0f}};
        const std::string module = std::string(argv[2]) + "/softclip.so";
        assert(softclip->prepare(module.c_str(), "plugin_softclip", drive, 1,
                                 48000, frames, 2, argv[1]));
        Audio clipped;
        clipped.params[0] = 1;
        clipped.fill(0.2f, -0.2f);
        clipped.run(*softclip);
        clipped.check(0, 0);
        settle();
        clipped.run(*softclip);
        clipped.check(0.2f / 1.2f, -0.2f / 1.2f);
        assert(host->status() == PluginHost::Status::Ready &&
               second->status() == PluginHost::Status::Ready);
    }
    // Reset discards both a completed response and the delayed dry copy.
    host->reset(); audio.fill(0, 0); audio.run(*host); audio.check(0, 0);
    assert(!host->hasPendingAudio(0.01f));
    settle(); audio.run(*host); audio.check(0, 0);
    settle(); audio.run(*host); audio.check(0, 0);
    // In-place buffers must capture input before overwriting the output.
    settle();
    host->reset(); audio.fill(0.2f, 0.3f);
    float* inplace[2]{audio.in[0], audio.in[1]};
    host->process(audio.inputs, inplace, frames, audio.params);
    settle(); audio.fill(0, 0);
    host->process(audio.inputs, inplace, frames, audio.params);
    assert(std::fabs(audio.in[0][0] - 0.4f) < 0.00001f);
    second->stop(); host->stop();
    {
        audiox::plugin::PackageMetadata metadata{};
        strcpy(metadata.id, "plugin_fixture");
        metadata.paramCount = AUDIOX_PLUGIN_MAX_PARAMS;
        for (unsigned i = 0; i < metadata.paramCount; ++i) {
            auto& p = metadata.params[i];
            snprintf(p.name, sizeof(p.name), "p%u", i);
            strcpy(p.label, "Parameter");
            p.min = -2; p.max = 2; p.defaultValue = 1;
        }
        std::string error;
        assert(host->prepare(path(12).c_str(), metadata, 48000, frames, 2, error, argv[1]));
        Audio wide;
        wide.params[AUDIOX_PLUGIN_MAX_PARAMS - 1] = 0.5f;
        wide.fill(0.2f, -0.2f);
        wide.run(*host);
        settle();
        wide.run(*host);
        wide.check(0.1f, -0.1f);
        host->stop();
    }
    {
        constexpr uint32_t block = audiox::plugin::kMaxFrames;
        constexpr uint32_t channels = audiox::plugin::kMaxChannels;
        auto input = std::make_unique<float[]>(channels * block);
        auto output = std::make_unique<float[]>(channels * block);
        const float* inputs[channels];
        float* outputs[channels];
        for (uint32_t c = 0; c < channels; ++c) {
            inputs[c] = input.get() + c * block;
            outputs[c] = output.get() + c * block;
            for (uint32_t f = 0; f < block; ++f) input[c * block + f] = (c + 1) * 0.01f;
        }
        assert(host->prepare(path(0).c_str(), "plugin_fixture", specs, 1,
                             48000, block, channels, argv[1]));
        host->process(inputs, outputs, block, audio.params);
        for (uint32_t i = 0; i < channels * block; ++i) assert(output[i] == 0);
        settle();
        float changed[8]{0.5f};
        host->process(inputs, outputs, block, changed);
        for (uint32_t i = 0; i < channels * block; ++i)
            assert(std::fabs(output[i] - input[i] * 2) < 0.00001f);
        settle();
        host->process(inputs, outputs, block, changed);
        for (uint32_t i = 0; i < channels * block; ++i)
            assert(std::fabs(output[i] - input[i] * 0.75f) < 0.00001f);
        host->stop();
        assert(!host->prepare(path(0).c_str(), "plugin_fixture", specs, 1,
                              48000, block + 1, channels, argv[1]));
        assert(!host->prepare(path(0).c_str(), "plugin_fixture", specs, 1,
                              48000, block, channels + 1, argv[1]));
    }
    assert(!host->prepare(path(0).c_str(), "wrong_id", specs, 1, 48000, frames, 2, argv[1]));
    assert(host->status() == PluginHost::Status::Fault && host->error()[0]);
    AudioxParameterV1 wrong[] = {{"gain", "Gain", -2, 3, 1}};
    assert(!host->prepare(path(0).c_str(), "plugin_fixture", wrong, 1, 48000, frames, 2, argv[1]));
    for (unsigned kind : {1u, 4u}) {
        assert(!prepare(*host, argv[1], path(kind)));
        assert(host->status() == PluginHost::Status::Fault);
    }
    assert(access("plugin-constructor-escape", F_OK) != 0);
    assert(prepare(*host, argv[1], path(7)));
    host->stop();
#ifdef AUDIOX_PLUGIN_NAMESPACE_TEST
    assert(prepare(*host, argv[1], path(11)));
    host->stop();
#endif
    assert(!prepare(*host, argv[1], path(8)));
    const auto startup = std::chrono::steady_clock::now();
    assert(!prepare(*host, argv[1], path(9)));
    assert(std::chrono::steady_clock::now() - startup < std::chrono::seconds(2));
    for (unsigned kind : {2u, 3u, 5u, 10u}) {
        assert(prepare(*host, argv[1], path(kind)));
        audio.fill(0.1f, 0.3f);
        audio.run(*host);
        settle();
        const auto before = std::chrono::steady_clock::now();
        for (unsigned i = 0; i < 64; ++i) audio.run(*host);
        const auto duration = std::chrono::steady_clock::now() - before;
        assert(duration < std::chrono::milliseconds(50));
        assert(host->status() == PluginHost::Status::Fault);
        audio.check(0.1f, 0.3f);
        host->poll(); assert(host->error()[0]);
        const auto stopping = std::chrono::steady_clock::now();
        host->stop();
        assert(std::chrono::steady_clock::now() - stopping < std::chrono::milliseconds(250));
    }
    // A late wet block is never played back after its designated output cycle.
    assert(prepare(*host, argv[1], path(6)));
    audio.fill(0.1f, 0.3f); audio.run(*host); audio.check(0, 0);
    audio.fill(0.2f, 0.4f); audio.run(*host); audio.check(0.1f, 0.3f);
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    audio.fill(0.3f, 0.5f); audio.run(*host); audio.check(0.2f, 0.4f);
    host->reset();
    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    audio.fill(0, 0); audio.run(*host); audio.check(0, 0);
    host->stop(); host.reset(); second.reset();
    int status;
    assert(waitpid(-1, &status, WNOHANG) == -1 && "all workers must be reaped");
    std::puts("plugin_host_test: ok");
#ifdef AUDIOX_PLUGIN_NAMESPACE_TEST
    struct stat current_namespace{};
    assert(stat("/proc/self/ns/mnt", &current_namespace) == 0);
    assert(current_namespace.st_ino == original_namespace.st_ino);
    assert(access("/tmp/audiox-plugin-secret", R_OK) == 0);
    assert(access("/tmp/module.so", F_OK) == -1);
    assert(umount("/proc") == 0);
    auto unsupported = std::make_unique<PluginHost>();
    assert(!prepare(*unsupported, argv[1], path(0)));
    assert(std::strstr(unsupported->error(), "Cannot bind sandbox path:") != nullptr);
    unsupported->stop();
    assert(mount("proc", "/proc", "proc", 0, nullptr) == 0);
    assert(unlink("/tmp/audiox-plugin-secret") == 0 && rmdir("/tmp") == 0);
    assert(symlink("/lib", "/tmp") == 0);
    assert(!prepare(*unsupported, argv[1], path(0)));
    assert(std::strstr(unsupported->error(), "Cannot create worker mount directory:") != nullptr);
    unsupported->stop();
    assert(unlink("/tmp") == 0 && mkdir("/tmp", 0755) == 0);
    std::puts("plugin_namespace_test: ok");
    std::fflush(stdout);
    assert(reboot(RB_POWER_OFF) == 0);
#endif
}
#endif
