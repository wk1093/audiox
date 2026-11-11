#include "audio/plugin_api.h"
#ifdef AUDIOX_PLUGIN_TEST_FIXTURE
#include <cstdlib>
#include <cstring>

namespace {
struct State { float previous[1024]; uint32_t frames; };
#if AUDIOX_PLUGIN_TEST_FIXTURE == 12
#define PARAMETER(n) {"p" #n, "Parameter", -2, 2, 1}
const AudioxParameterV1 parameters[] = {
    PARAMETER(0), PARAMETER(1), PARAMETER(2), PARAMETER(3),
    PARAMETER(4), PARAMETER(5), PARAMETER(6), PARAMETER(7),
    PARAMETER(8), PARAMETER(9), PARAMETER(10), PARAMETER(11),
    PARAMETER(12), PARAMETER(13), PARAMETER(14), PARAMETER(15),
    PARAMETER(16), PARAMETER(17), PARAMETER(18), PARAMETER(19),
    PARAMETER(20), PARAMETER(21), PARAMETER(22), PARAMETER(23),
    PARAMETER(24), PARAMETER(25), PARAMETER(26), PARAMETER(27),
    PARAMETER(28), PARAMETER(29), PARAMETER(30), PARAMETER(31)
};
#undef PARAMETER
#else
const AudioxParameterV1 parameters[] = {{"gain", "Gain", -2, 2, 1}};
#endif
#if AUDIOX_PLUGIN_TEST_FIXTURE == 4
volatile unsigned initialized;
__attribute__((constructor)) void constructor() { initialized = 1; }
#endif
#if AUDIOX_PLUGIN_TEST_FIXTURE == 5
extern "C" void audiox_missing_import();
#endif
void *create(uint32_t, uint32_t) {
#if AUDIOX_PLUGIN_TEST_FIXTURE == 7
    return nullptr;
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 9
    for (;;) asm volatile("" ::: "memory");
#else
    return std::calloc(1, sizeof(State));
#endif
}
void destroy(void *instance) { std::free(instance); }
void reset(void *instance) { std::memset(instance, 0, sizeof(State)); }
void process(void *instance, const float *input, float *output, uint32_t frames, const float *params) {
#if AUDIOX_PLUGIN_TEST_FIXTURE == 2
    std::abort();
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 3
    for (;;) asm volatile("" ::: "memory");
#elif AUDIOX_PLUGIN_TEST_FIXTURE == 5
    audiox_missing_import();
#endif
    auto *state = static_cast<State *>(instance);
    for (uint32_t i = 0; i < frames; ++i)
        output[i] = input[i] * params[AUDIOX_PLUGIN_TEST_FIXTURE == 12 ? 31 : 0] +
                    state->previous[i] * 0.25f;
    std::memcpy(state->previous, input, frames * sizeof(float));
    state->frames = frames;
}
int tail(void *instance, float threshold) {
    auto *state = static_cast<State *>(instance);
    for (uint32_t i = 0; i < state->frames; ++i)
        if (state->previous[i] > threshold || state->previous[i] < -threshold) return 1;
    return 0;
}
const AudioxPluginV1 descriptor = {
    AUDIOX_PLUGIN_TEST_FIXTURE == 1 ? 99U : AUDIOX_PLUGIN_ABI_VERSION,
    sizeof(AudioxPluginV1), AUDIOX_PLUGIN_TEST_FIXTURE == 6 ? "plugin_other" : "plugin_fixture",
    AUDIOX_PLUGIN_TEST_FIXTURE == 12 ? 32U : 1U, parameters, create, destroy, reset, process, tail
};
}
extern "C" const AudioxPluginV1 *audiox_plugin_v1() { return &descriptor; }
#else
#include "audio/plugin_host.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

using namespace audiox::plugin;
int main(int argc, char **argv) {
    assert(argc == 3);
    const char *compiler = argv[1];
    const std::string directory = argv[2];
    const auto path = [&](unsigned fixture) {
        return directory + "/plugin-fixture-" + std::to_string(fixture) + ".bc";
    };
    const AudioxParameterV1 parameter{"gain", "Gain", -2, 2, 1};
    const auto prepare = [&](PluginHost &host, unsigned fixture, const char *id = "plugin_fixture") {
        return host.prepare(path(fixture).c_str(), id, &parameter, 1, 48000, 128, 2, compiler);
    };
    assert(!pluginExecutionEnabled());
    {
        PluginHost control;
        // These entry points cannot safely execute in the application.
        assert(prepare(control, 1));
        assert(control.status() == PluginHost::Status::Ready);
        assert(prepare(control, 2));
        assert(prepare(control, 3));
        assert(prepare(control, 9));
    }
    setPluginExecutionEnabled(true);
    for (unsigned fixture : {1U, 4U, 5U, 6U, 7U}) {
        PluginHost rejected;
        assert(!prepare(rejected, fixture));
        assert(rejected.status() == PluginHost::Status::Fault);
        assert(std::strlen(rejected.error()));
        if (fixture == 5) assert(std::strstr(rejected.error(), "audiox_missing_import"));
        if (fixture == 4) assert(std::strstr(rejected.error(), "constructors"));
    }
    {
        PluginHost absent;
        assert(!absent.prepare(path(0).c_str(), "plugin_fixture", &parameter, 1,
                               48000, 128, 2, "/nonexistent/audiox-graph-compiler"));
        assert(std::strstr(absent.error(), "unavailable"));
    }
    const std::string corrupt = directory + "/invalid.bc";
    {
        std::ofstream output(corrupt, std::ios::binary);
        output.write("BC\300\336garbage", 11);
    }
    {
        PluginHost invalid;
        assert(!invalid.prepare(corrupt.c_str(), "plugin_fixture", &parameter, 1,
                                48000, 128, 2, compiler));
        assert(invalid.status() == PluginHost::Status::Fault);
    }
    assert(!unlink(corrupt.c_str()));
    PluginHost host;
    assert(prepare(host, 0));
    const uint64_t revision = host.revision();
    assert(prepare(host, 0) && host.revision() > revision);
    float left[128]{}, right[128]{}, outLeft[128]{}, outRight[128]{};
    const float *input[]{left, right};
    float *output[]{outLeft, outRight};
    float params[AUDIOX_PLUGIN_MAX_PARAMS]{};
    params[0] = 2;
    left[0] = 0.5f;
    right[0] = -0.75f;
    host.process(input, output, 128, params);
    // Same-block processing: no initial silence and no delayed dry path.
    assert(outLeft[0] == 1 && outRight[0] == -1.5f);
    assert(host.hasPendingAudio(0.00001f));
    assert(!host.hasPendingAudio(1.0f));
    left[0] = right[0] = 0;
    host.process(input, output, 128, params);
    assert(outLeft[0] == 0.125f && outRight[0] == -0.1875f);
    assert(!host.hasPendingAudio(0.00001f));
    left[0] = 1;
    host.process(input, output, 128, params);
    host.reset();
    assert(!host.hasPendingAudio(0.00001f));
    left[0] = 0;
    host.process(input, output, 128, params);
    assert(outLeft[0] == 0);
    {
        std::string names[32];
        AudioxParameterV1 specifications[32]{};
        for (unsigned i = 0; i < 32; ++i) {
            names[i] = "p" + std::to_string(i);
            specifications[i] = {names[i].c_str(), "Parameter", -2, 2, 1};
        }
        PluginHost wide;
        assert(wide.prepare(path(12).c_str(), "plugin_fixture", specifications, 32,
                            48000, 128, 2, compiler));
        params[31] = 1.5f;
        left[0] = 0.5f;
        wide.process(input, output, 128, params);
        assert(outLeft[0] == 0.75f);
    }
    setPluginExecutionEnabled(false);
    left[0] = 0.75f;
    host.process(input, output, 128, params);
    assert(outLeft[0] == left[0]);
    assert(!host.hasPendingAudio(0.00001f));
    setPluginExecutionEnabled(true);
    // Plugin faults kill only the process that opted into execution.
    const pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        PluginHost crashing;
        if (!prepare(crashing, 2)) _exit(10);
        crashing.process(input, output, 128, params);
        _exit(11);
    }
    int status{};
    assert(waitpid(child, &status, 0) == child);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
    std::puts("plugin_host_test: ok (worker-local bitcode, synchronous processing)");
}
#endif
