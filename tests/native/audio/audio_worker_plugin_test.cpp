#include "audio/worker_protocol.hpp"
#include "audio/plugin_registry.hpp"
#include "../script_device_stubs.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <sys/resource.h>
#include <thread>
#include <unistd.h>

size_t BluetoothContext::copyRoutingDevices(BluetoothRoutingDevice *, size_t) const { return 0; }

static void until(const std::function<bool()> &condition,
                  const std::function<void()> &service, unsigned timeoutMs = 15000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        service();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc == 3)
        return audiox::worker::run(std::stoi(argv[1]), std::stoi(argv[2]));
    assert(argc == 2);
    const rlimit noCore{0, 0};
    assert(setrlimit(RLIMIT_CORE, &noCore) == 0);
    const auto executable = std::filesystem::canonical(argv[0]).string();
    const auto fixtures = std::filesystem::canonical(argv[1]).string();
    const auto previous = std::filesystem::current_path();
    char temporary[] = "/tmp/audiox-worker-plugin-XXXXXX";
    assert(mkdtemp(temporary));
    assert(chdir(temporary) == 0);
    const auto install = [&](unsigned fixture) {
        using namespace audiox::plugin;
        const std::string metadata =
            "id=plugin_fixture\nname=Worker fixture\nversion=1\napi=1\nparam=gain|Gain|-2|2|1\n";
        std::ifstream input(fixtures + "/plugin-fixture-" + std::to_string(fixture) + ".bc",
                            std::ios::binary);
        assert(input);
        const std::vector<uint8_t> module{std::istreambuf_iterator<char>(input), {}};
        std::vector<Entry> entries{
            {Metadata, "axm.txt", {metadata.begin(), metadata.end()}},
            {Module, "module.bc", module}};
        std::string error;
        if (std::filesystem::exists(std::string(ROOT) + "/plugin_fixture"))
            assert(removePackage("plugin_fixture", ROOT, error));
        std::vector<uint8_t> package;
        assert(buildPackage(entries, package, error));
        assert(installPackage(package.data(), package.size(), ROOT, error));
        assert(loadCatalog(error));
    };
    install(0);
    assert(!audiox::plugin::pluginExecutionEnabled());
    {
        auto ctx = std::make_unique<AudioContext>(nullptr, false);
        const auto type = audiox::effects::effectTypeFromString("plugin_fixture");
        auto &effect = ctx->effectStates["worker_plugin"];
        effect.type = type;
        effect.enabled = 1;
        effect.values[0] = 0.5f;
        auto host = ctx->preparePlugin("worker_plugin", type);
        assert(host && !audiox::plugin::pluginExecutionEnabled());
        ctx->attachPlugin("worker_plugin", effect, std::move(host));
        auto &builtin = ctx->effectStates["builtin_gain"];
        builtin.type = audiox::effects::effectTypeFromString("gain");
        builtin.enabled = 1;
        builtin.values[0] = 0.5f;
        auto &graph = ctx->routingGraphPublished;
        graph.generation = 1;
        graph.thingCount = 4;
        strcpy(graph.things[0].id, "bt_112233445566_in");
        graph.things[0].outputs = 2;
        strcpy(graph.things[1].id, "worker_plugin");
        graph.things[1].inputs = graph.things[1].outputs = 2;
        strcpy(graph.things[2].id, "builtin_gain");
        graph.things[2].inputs = graph.things[2].outputs = 2;
        strcpy(graph.things[3].id, "bt_112233445566_out");
        graph.things[3].inputs = 2;
        for (unsigned from = 0; from < 3; ++from) {
            for (unsigned channel = 0; channel < 2; ++channel) {
                auto &edge = graph.edges[graph.edgeCount++];
                strcpy(edge.src, graph.things[from].id);
                strcpy(edge.dst, graph.things[from + 1].id);
                edge.srcChannel = edge.dstChannel = channel;
            }
        }
        AudioWorkerSupervisor supervisor(*ctx, executable.c_str());
        assert(supervisor.start());
        ctx->bluetoothInputs[0].start(0x112233445566ULL, SAMPLE_RATE);
        ctx->bluetoothOutputs[0].peer.store(0x112233445566ULL);
        ctx->bluetoothOutputs[0].setActive(true);
        const auto service = [&] {
            int16_t pcm[512 * 2];
            std::fill_n(pcm, 512 * 2, 4096);
            ctx->bluetoothInputs[0].push(pcm, 512);
            (void)ctx->bluetoothOutputs[0].available();
            ctx->pollPlugins();
        };
        const auto levelIs = [&](float expected) {
            return std::fabs(ctx->nodeChannelLevels[1][0].load() - expected) < 0.01f &&
                   std::fabs(ctx->nodeChannelLevels[2][0].load() - expected * 0.5f) < 0.005f;
        };
        until([&] { return levelIs(0.09375f); }, service);
        const auto initialStatus = supervisor.statusJson();
        {
            std::lock_guard<std::mutex> lock(ctx->effectsMutex);
            effect.values[0] = 1.5f;
            ctx->effectStatesSeq.fetch_add(1, std::memory_order_release);
        }
        until([&] { return levelIs(0.21875f); }, service);
        assert(supervisor.statusJson() == initialStatus);
        const auto runtimeIs = [&](audiox::plugin::PluginHost::Status status, const char *error) {
            std::lock_guard<std::mutex> lock(ctx->effectsMutex);
            const auto found = ctx->pluginRuntimeStates.find("worker_plugin");
            return found != ctx->pluginRuntimeStates.end() &&
                   found->second.status == status && strstr(found->second.error, error);
        };
        install(7);
        assert(ctx->reloadPlugins("plugin_fixture") == RET_OK);
        until([&] {
            return runtimeIs(audiox::plugin::PluginHost::Status::Fault, "create failed") &&
                   levelIs(0.125f);
        }, service);
        install(0);
        assert(ctx->reloadPlugins("plugin_fixture") == RET_OK);
        until([&] {
            return runtimeIs(audiox::plugin::PluginHost::Status::Ready, "") && levelIs(0.21875f);
        }, service);
        // Same-type reload must reach the worker without an explicit audio restart.
        install(2);
        assert(ctx->reloadPlugins("plugin_fixture") == RET_OK);
        until([&] {
            const auto status = supervisor.statusJson();
            return status.find("\"plugins_disabled\":true") != std::string::npos &&
                   status.find("\"running\":true") != std::string::npos && levelIs(0.125f);
        }, service);
        assert(effect.enabled && effect.values[0] == 1.5f);
        assert(runtimeIs(audiox::plugin::PluginHost::Status::Stopped, "All plugins bypassed"));
        assert(!audiox::plugin::pluginExecutionEnabled());
        assert(supervisor.restart(false) == RET_OK);
        until([&] {
            const auto status = supervisor.statusJson();
            return status.find("\"plugins_disabled\":true") != std::string::npos &&
                   status.find("\"running\":true") != std::string::npos;
        }, service);
        install(0);
        assert(ctx->reloadPlugins("plugin_fixture") == RET_OK);
        assert(supervisor.restart(true) == RET_OK);
        until([&] {
            const auto status = supervisor.statusJson();
            return status.find("\"plugins_disabled\":false") != std::string::npos &&
                   status.find("\"running\":true") != std::string::npos && levelIs(0.21875f);
        }, service);
        {
            std::lock_guard<std::mutex> lock(ctx->effectsMutex);
            ctx->retirePlugin("worker_plugin");
            effect.pluginHost = nullptr;
            effect.type = audiox::effects::effectTypeFromString("gain");
            effect.enabled = 0;
            ctx->effectStatesSeq.fetch_add(1, std::memory_order_release);
        }
        until([&] { return levelIs(0.125f); }, service);
    }
    assert(chdir(previous.c_str()) == 0);
    std::filesystem::remove_all(temporary);
    puts("Exec audio worker: native builtins, live bitcode, load errors, parameter replay, reload, real plugin crash bypass, retry and type transition passed");
}
