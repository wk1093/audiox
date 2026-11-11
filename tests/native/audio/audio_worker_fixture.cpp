#include "audio/worker_protocol.hpp"
#include "audio/health.hpp"
#include <cassert>
#include <cstdlib>
#include <signal.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

int main(int argc, char **argv) {
    assert(argc == 3);
    assert(prctl(PR_SET_PDEATHSIG, SIGKILL) == 0);
    auto *shared = static_cast<audiox::worker::Shared *>(mmap(nullptr, sizeof(audiox::worker::Shared),
        PROT_READ | PROT_WRITE, MAP_SHARED, atoi(argv[1]), 0));
    assert(shared != MAP_FAILED && shared->magic == audiox::worker::kMagic);
    assert(getppid() == atoi(argv[2]));
    audiox::worker::Control control;
    assert(shared->control.consume(control));
    shared->ready.store(true, std::memory_order_release);
    while (!shared->stop.load(std::memory_order_acquire)) {
        shared->control.consume(control);
        if (control.graph.generation == 2 && !control.pluginsDisabled) return 7;
        if (control.graph.generation == 3) { usleep(10000); continue; }
        audiox::worker::Telemetry telemetry;
        telemetry.graphGeneration = control.graph.generation;
        telemetry.devices.sampledAt = audiox::healthMonotonicMs();
        telemetry.devices.count = 1;
        auto &stream = telemetry.devices.streams[0];
        snprintf(stream.id, sizeof(stream.id), "%s", control.graph.things[0].id);
        stream.rate = static_cast<unsigned>(control.gains[0] * 1000);
        stream.active = control.effectCount && control.effects[0].enabled && control.effects[0].values[0] == 0.25f;
        telemetry.bufferRuntime = getpid();
        telemetry.xruns = 2;
        telemetry.silent = control.muted;
        telemetry.processedSceneEpoch = control.sceneEpoch;
        telemetry.releasedSceneEpoch = control.releaseGeneration ? control.sceneEpoch : 0;
        if (control.graph.generation == 4) telemetry.playingCount = AUDIO_SFX_SLOT_COUNT + 1;
        shared->telemetry.publish(telemetry);
        const auto now = audiox::healthMonotonicMs();
        shared->heartbeat.store(now, std::memory_order_release);
        shared->controlHeartbeat.store(now, std::memory_order_release);
        unsigned read = shared->soundRead.load(std::memory_order_relaxed);
        const unsigned end = shared->soundWrite.load(std::memory_order_acquire);
        while (read != end) {
            const auto &sound = shared->sounds[read++ % AUDIO_SFX_QUEUE_CAP];
            assert(sound.sampleCount == 2 && shared->pcm[sound.trigger.slotIndex][0] == 1234);
            assert(shared->references[sound.trigger.slotIndex].fetch_sub(1) > 0);
        }
        shared->soundRead.store(read, std::memory_order_release);
        usleep(2000);
    }
}
