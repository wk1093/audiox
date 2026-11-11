#include "audio/worker_protocol.hpp"
#include "audio/health.hpp"
#include <cassert>
#include <chrono>
#include <cstring>
#include <functional>
#include <memory>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

AudioContext::AudioContext(Audiox *context) : app(context) {
    memset(sfxSlots, 0, sizeof(sfxSlots));
    routingGraphPublished = {};
    sfxQueueRead.store(0);
    sfxQueueWrite.store(0);
    pendingSfxStopAll.store(0);
    pendingSfxHoldStops.store(0);
    sfxPlayingCount = 0;
    memset(sfxPlayingBasenames, 0, sizeof(sfxPlayingBasenames));
    effectStatesSeq.store(5);
}
AudioContext::~AudioContext() {}
audiox::plugin::PluginHost::~PluginHost() {}
size_t AudioContext::copyDeviceInfos(AudioDeviceInfo *, size_t) const { return 0; }
namespace audiox::effects {
const char *effectTypeToString(uint8_t) { return "gain"; }
}

static void until(const std::function<bool()> &condition, unsigned timeoutMs = 8000) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (!condition()) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main(int argc, char **argv) {
    assert(argc == 2);
    using audiox::worker::RecoveryPolicy;
    RecoveryPolicy recovery;
    recovery.failed(1000);
    assert(recovery.failures == 1 && recovery.retryAt == 1250 && !recovery.pluginsDisabled);
    recovery.failed(1250);
    assert(recovery.retryAt == 1750);
    recovery.failed(1750);
    assert(recovery.retryAt == 2750 && recovery.pluginsDisabled);
    recovery.stable(60000, 1);
    assert(recovery.failures == 3);
    recovery.stable(60001, 1);
    assert(recovery.failures == 0 && recovery.pluginsDisabled);
    recovery.manual(false);
    assert(recovery.pluginsDisabled);
    recovery.manual(true);
    assert(!recovery.pluginsDisabled && !recovery.failures);

    auto ctx = std::make_unique<AudioContext>(nullptr);
    ctx->routingGraphPublished.generation = 1;
    ctx->routingGraphPublished.thingCount = 1;
    strcpy(ctx->routingGraphPublished.things[0].id, "preserved_route");
    ctx->thingGains["preserved_route"] = 0.75f;
    auto &effect = ctx->effectStates["effect"];
    effect.enabled = 1;
    effect.values[0] = 0.25f;
    pid_t finalPid = -1;
    {
        AudioWorkerSupervisor supervisor(*ctx, argv[1]);
        assert(supervisor.start());
        until([&] { return supervisor.statusJson().find("\"running\":true") != std::string::npos; });
        until([&] {
            const auto &data = ctx->deviceHealth.read();
            return data.count && data.streams[0].rate == 750 && data.streams[0].active &&
                   !strcmp(data.streams[0].id, "preserved_route");
        });
        assert(audiox::audioHealth.total() == 2);
        {
            std::lock_guard<std::mutex> bank(ctx->sfxBankMutex);
            static int16_t samples[] = {1234, 5678};
            auto &clip = ctx->sfxSlots[0];
            clip.loaded = 1;
            clip.pcm = samples;
            clip.frames = 1;
            clip.sampleCount = 2;
            clip.channels = 2;
            clip.sampleRate = 48000;
            ctx->sfxBankRevision[0] = 1;
            ctx->sfxSlotRefs[0].store(1);
            ctx->sfxQueue[0] = {0, 0, 1};
            ctx->sfxQueueWrite.store(1, std::memory_order_release);
        }
        until([&] { return ctx->sfxSlotRefs[0].load() == 0; });
        {
            std::lock_guard<std::mutex> graph(ctx->routingGraphMutex);
            ctx->routingGraphPublished.generation = 2;
        }
        until([&] {
            const auto status = supervisor.statusJson();
            return status.find("\"plugins_disabled\":true") != std::string::npos &&
                   status.find("\"running\":true") != std::string::npos;
        });
        assert(ctx->thingGains["preserved_route"] == 0.75f && ctx->effectStates["effect"].enabled);
        assert(audiox::audioHealth.total() >= 4);
        {
            std::lock_guard<std::mutex> graph(ctx->routingGraphMutex);
            ctx->routingGraphPublished.generation = 1;
        }
        assert(supervisor.restart(false) == RET_OK);
        until([&] {
            const auto status = supervisor.statusJson();
            return status.find("\"consecutive_failures\":0") != std::string::npos &&
                   status.find("\"plugins_disabled\":true") != std::string::npos;
        });
        assert(supervisor.restart(true) == RET_OK);
        until([&] {
            const auto status = supervisor.statusJson();
            return status.find("\"plugins_disabled\":false") != std::string::npos &&
                   status.find("\"running\":true") != std::string::npos;
        });
        {
            std::lock_guard<std::mutex> graph(ctx->routingGraphMutex);
            ctx->routingGraphPublished.generation = 3;
        }
        until([&] { return supervisor.statusJson().find("\"last_exit_status\":-110") != std::string::npos; });
        {
            std::lock_guard<std::mutex> graph(ctx->routingGraphMutex);
            ctx->routingGraphPublished.generation = 1;
        }
        assert(supervisor.restart(false) == RET_OK);
        until([&] { return supervisor.statusJson().find("\"running\":true") != std::string::npos; });
        {
            std::lock_guard<std::mutex> graph(ctx->routingGraphMutex);
            ctx->routingGraphPublished.generation = 4;
        }
        until([&] { return supervisor.statusJson().find("\"last_exit_status\":-71") != std::string::npos; });
        {
            std::lock_guard<std::mutex> graph(ctx->routingGraphMutex);
            ctx->routingGraphPublished.generation = 1;
        }
        assert(supervisor.restart(false) == RET_OK);
        until([&] { return supervisor.statusJson().find("\"running\":true") != std::string::npos; });
        const auto status = supervisor.statusJson();
        const auto pid = status.find("\"pid\":");
        finalPid = std::stoi(status.substr(pid + 6));
    }
    assert(ctx->sfxSlotRefs == ctx->localSfxSlotRefs && ctx->bluetoothInputs == ctx->localBluetoothInputs);
    int status{};
    assert(waitpid(finalPid, &status, WNOHANG) == -1 && errno == ECHILD);
    puts("Audio worker exec, transport, configuration replay, crash bypass, manual restart and hang recovery passed");
}
