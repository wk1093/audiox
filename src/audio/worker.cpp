#include "audio/worker_protocol.hpp"
#include "audio/health.hpp"
#include "audio/stress_test.hpp"
#include "audio/plugin_registry.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <linux/memfd.h>
#include <new>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unordered_set>
#include <unistd.h>

extern char **environ;

using audiox::worker::Shared;
using audiox::worker::Control;
using audiox::worker::Telemetry;

struct AudioWorkerSupervisor::State {
    AudioContext &context;
    std::string path;
    Shared *shared{};
    int fd{-1};
    pid_t pid{-1};
    std::atomic<bool> running{false}, restartRequested{false}, enableRequested{false};
    std::atomic<bool> ready{false}, disabled{false};
    std::atomic<uint32_t> restartCount{0}, consecutiveFailures{0};
    std::atomic<int> publicPid{-1}, lastExit{0};
    audiox::worker::RecoveryPolicy recovery;
    uint64_t startedAt{}, lastXruns{};
    uint64_t lastBufferRuntime{}, lastBufferChange{};
    audiox::DeviceHealthSnapshot lastDevices{};
    uint64_t copiedBank[AUDIO_SFX_SLOT_COUNT]{};
    uint32_t lastScriptDropped{};
    std::unordered_set<std::string> sounding;

    State(AudioContext &ctx, const char *workerPath) : context(ctx), path(workerPath) {}

    bool map() {
        fd = static_cast<int>(syscall(SYS_memfd_create, "audiox-audio", MFD_CLOEXEC | MFD_ALLOW_SEALING));
        if (fd < 0 || ftruncate(fd, sizeof(Shared)) < 0) return false;
        void *memory = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (memory == MAP_FAILED) return false;
        shared = new (memory) Shared;
        if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL) < 0) return false;
        context.sfxSlotRefs = shared->references;
        context.bluetoothInputs = shared->bluetoothInputs;
        context.bluetoothOutputs = shared->bluetoothOutputs;
        return true;
    }

    void publishControl() {
        Control control;
        {
            std::lock_guard<std::mutex> graph(context.routingGraphMutex);
            control.graph = context.routingGraphPublished;
        }
        {
            std::lock_guard<std::mutex> gains(context.gainsMutex);
            for (unsigned i = 0; i < control.graph.thingCount; ++i) {
                const auto found = context.thingGains.find(control.graph.things[i].id);
                control.gains[i] = found == context.thingGains.end() ? 1.0f : found->second;
            }
        }
        control.deviceCount = context.copyDeviceInfos(control.devices, 64);
        {
            std::lock_guard<std::mutex> effects(context.effectsMutex);
            control.effectSequence = context.effectStatesSeq.load(std::memory_order_acquire);
            for (const auto &entry : context.effectStates) {
                if (control.effectCount >= AUDIO_GRAPH_MAX_THINGS) break;
                auto &effect = control.effects[control.effectCount++];
                snprintf(effect.id, sizeof(effect.id), "%s", entry.first.c_str());
                snprintf(effect.type, sizeof(effect.type), "%s",
                         audiox::effects::effectTypeToString(entry.second.type));
                effect.enabled = entry.second.enabled;
                effect.revision = entry.second.pluginHost ? entry.second.pluginHost->revision() : 0;
                std::copy_n(entry.second.values, audiox::effects::EFFECT_PARAM_MAX, effect.values);
            }
        }
        control.captureGain = context.gadgetVolume.captureGain.load(std::memory_order_acquire);
        control.playbackGain = context.gadgetVolume.playbackGain.load(std::memory_order_acquire);
        control.muted = context.sceneMuted.load(std::memory_order_acquire);
        control.sceneEpoch = context.sceneMuteEpoch.load(std::memory_order_acquire);
        control.releaseGeneration = context.sceneReleaseGeneration.load(std::memory_order_acquire);
        control.pluginsDisabled = recovery.pluginsDisabled;
        control.stressExpiryNs = audiox::audioStressTest.audioExpiryNs();
        control.stressWorkNs = audiox::audioStressTest.audioWorkNs();
        if (audiox::audioStressTest.mode() == audiox::AudioStressTest::AudioThread &&
            !audiox::audioStressTest.remainingMs()) audiox::audioStressTest.stop();
        shared->control.publish(control);
    }

    void publishSounds() {
        std::lock_guard<std::mutex> bank(context.sfxBankMutex);
        uint32_t read = context.sfxQueueRead.load(std::memory_order_relaxed);
        const uint32_t end = context.sfxQueueWrite.load(std::memory_order_acquire);
        uint32_t write = shared->soundWrite.load(std::memory_order_relaxed);
        while (read != end && write - shared->soundRead.load(std::memory_order_acquire) < AUDIO_SFX_QUEUE_CAP) {
            auto &sound = shared->sounds[write % AUDIO_SFX_QUEUE_CAP];
            sound = {};
            sound.trigger = context.sfxQueue[read % AUDIO_SFX_QUEUE_CAP];
            const unsigned slot = sound.trigger.slotIndex;
            if (slot >= AUDIO_SFX_SLOT_COUNT) {
                printf("[AUDIO] [ERR] invalid soundboard queue slot %u\n", slot);
                ++read;
                continue;
            }
            const auto &clip = context.sfxSlots[slot];
            sound.revision = context.sfxBankRevision[slot];
            snprintf(sound.name, sizeof(sound.name), "%s", clip.name);
            sound.frames = clip.frames;
            sound.sampleRate = clip.sampleRate;
            sound.sampleCount = clip.sampleCount;
            sound.channels = clip.channels;
            if (copiedBank[slot] != sound.revision) {
                std::copy_n(clip.pcm, clip.sampleCount, shared->pcm[slot]);
                copiedBank[slot] = sound.revision;
            }
            ++read;
            ++write;
            shared->soundWrite.store(write, std::memory_order_release);
        }
        context.sfxQueueRead.store(read, std::memory_order_release);
        if (context.pendingSfxStopAll.exchange(0, std::memory_order_acq_rel))
            shared->stopAll.fetch_add(1, std::memory_order_release);
        if (context.pendingSfxHoldStops.exchange(0, std::memory_order_acq_rel))
            shared->stopHeld.fetch_add(1, std::memory_order_release);
    }

    bool readTelemetry() {
        Telemetry data;
        if (shared->telemetry.consume(data)) {
            if (data.pluginCount > AUDIO_GRAPH_MAX_THINGS ||
                data.devices.count > audiox::DeviceHealthSnapshot::kMaxStreams ||
                data.playingCount > AUDIO_SFX_SLOT_COUNT ||
                data.activeStreams > audiox::DeviceHealthSnapshot::kMaxStreams ||
                data.bufferLevel > audiox::adaptiveAudioBuffer.maxLevel(BUFFER_FRAMES) ||
                data.xruns < lastXruns || data.xruns - lastXruns > 100000) {
                printf("[AUDIO] [ERR] invalid audio worker telemetry; restarting worker\n");
                return false;
            }
            {
                std::lock_guard<std::mutex> effects(context.effectsMutex);
                context.pluginRuntimeStates.clear();
                for (unsigned i = 0; i < data.pluginCount; ++i) {
                    auto &plugin = data.plugins[i];
                    plugin.id[sizeof(plugin.id) - 1] = '\0';
                    plugin.runtime.error[sizeof(plugin.runtime.error) - 1] = '\0';
                    if (plugin.runtime.status > audiox::plugin::PluginHost::Status::Fault) {
                        printf("[AUDIO] [ERR] invalid plugin status telemetry; restarting worker\n");
                        return false;
                    }
                    const auto found = context.effectStates.find(plugin.id);
                    if (found != context.effectStates.end() &&
                        audiox::plugin::isPluginType(found->second.type) &&
                        plugin.runtime.revision ==
                            (found->second.pluginHost ? found->second.pluginHost->revision() : 0))
                        context.pluginRuntimeStates[plugin.id] = plugin.runtime;
                }
            }
            for (unsigned i = 0; i < data.devices.count; ++i)
                data.devices.streams[i].id[sizeof(data.devices.streams[i].id) - 1] = '\0';
            for (unsigned i = 0; i < data.playingCount; ++i)
                data.playing[i][MIDI_SFX_PATH_MAX - 1] = '\0';
            context.deviceHealth.publish(data.devices);
            lastDevices = data.devices;
            context.processingGraphGeneration.store(data.graphGeneration, std::memory_order_release);
            for (unsigned n = 0; n < AUDIO_GRAPH_MAX_THINGS; ++n)
                for (unsigned ch = 0; ch < 16; ++ch)
                    context.nodeChannelLevels[n][ch].store(data.meters[n][ch], std::memory_order_relaxed);
            audiox::audioHealth.recordWorkerXruns(data.xruns - lastXruns);
            lastXruns = data.xruns;
            if (data.bufferRuntime != lastBufferRuntime || data.bufferChange != lastBufferChange) {
                audiox::audioHealth.bufferChanged();
                lastBufferRuntime = data.bufferRuntime;
                lastBufferChange = data.bufferChange;
            }
            audiox::adaptiveAudioBuffer.mirrorWorker(data.bufferRuntime, data.bufferLevel,
                data.activeStreams, data.largestPeriod, data.largestBuffer, data.bufferChange,
                data.bufferIncrease, data.downshiftDelay, data.downshiftFailures);
            context.pluginObservedSeq.store(data.pluginSequence, std::memory_order_release);
            const auto sceneEpoch = context.sceneMuteEpoch.load(std::memory_order_acquire);
            if (data.processedSceneEpoch == sceneEpoch)
                context.sceneSilent.store(data.silent, std::memory_order_release);
            uint32_t release = context.sceneReleaseGeneration.load(std::memory_order_acquire);
            if (release && data.graphGeneration >= release && data.releasedSceneEpoch == sceneEpoch &&
                context.sceneReleaseGeneration.compare_exchange_strong(release, 0, std::memory_order_acq_rel))
                context.sceneMuted.store(false, std::memory_order_release);
            std::lock_guard<std::mutex> playing(context.sfxPlayingMutex);
            if (context.sfxPlayingCount != data.playingCount ||
                memcmp(context.sfxPlayingBasenames, data.playing, sizeof(data.playing))) {
                context.sfxPlayingCount = data.playingCount;
                memcpy(context.sfxPlayingBasenames, data.playing, sizeof(data.playing));
                context.sfxPlayingSeq.fetch_add(1, std::memory_order_release);
            }
            context.sfxIsPlaying.store(data.playingCount != 0, std::memory_order_release);
        }
        context.processingProgressAtMs.store(shared->heartbeat.load(std::memory_order_acquire),
                                             std::memory_order_release);
        unsigned read = shared->scriptRead.load(std::memory_order_relaxed);
        const unsigned write = shared->scriptWrite.load(std::memory_order_acquire);
        if (write - read > 128) {
            printf("[AUDIO] [ERR] invalid worker script-event queue; restarting worker\n");
            return false;
        }
        while (read != write) {
            auto event = shared->scriptSounds[read++ % 128];
            event.sound[sizeof(event.sound) - 1] = '\0';
            context.publishScriptSoundEvent(event.sound, event.started);
            if (event.started) sounding.insert(event.sound);
            else sounding.erase(event.sound);
        }
        shared->scriptRead.store(read, std::memory_order_release);
        const auto dropped = shared->scriptDropped.load(std::memory_order_acquire);
        if (dropped != lastScriptDropped) {
            printf("[AUDIO] [WARN] worker sound-event transport dropped %u events\n", dropped - lastScriptDropped);
            lastScriptDropped = dropped;
        }
        return true;
    }

    void resetAfterExit() {
        std::lock_guard<std::mutex> lifecycle(context.workerStateMutex);
        std::lock_guard<std::mutex> queue(context.sfxQueueMutex);
        std::lock_guard<std::mutex> bank(context.sfxBankMutex);
        context.sfxQueueRead.store(context.sfxQueueWrite.load(std::memory_order_acquire),
                                   std::memory_order_release);
        for (auto &ref : shared->references) ref.store(0, std::memory_order_relaxed);
        for (auto &output : shared->bluetoothOutputs) {
            output.signalActive.store(0, std::memory_order_release);
            output.flushRequested.store(true, std::memory_order_release);
        }
        {
            std::lock_guard<std::mutex> playing(context.sfxPlayingMutex);
            for (unsigned i = 0; i < context.sfxPlayingCount; ++i) {
                context.publishScriptSoundEvent(context.sfxPlayingBasenames[i], false);
                sounding.erase(context.sfxPlayingBasenames[i]);
            }
            for (const auto &sound : sounding) context.publishScriptSoundEvent(sound.c_str(), false);
            sounding.clear();
            context.sfxPlayingCount = 0;
            memset(context.sfxPlayingBasenames, 0, sizeof(context.sfxPlayingBasenames));
            context.sfxPlayingSeq.fetch_add(1, std::memory_order_release);
        }
        context.sfxIsPlaying.store(0, std::memory_order_release);
        context.sceneSilent.store(false, std::memory_order_release);
        context.processingGraphGeneration.store(0, std::memory_order_release);
        {
            std::lock_guard<std::mutex> effects(context.effectsMutex);
            context.pluginRuntimeStates.clear();
        }
        lastDevices.sampledAt = audiox::healthMonotonicMs();
        for (unsigned i = 0; i < lastDevices.count; ++i) {
            lastDevices.streams[i].active = false;
            lastDevices.streams[i].rate = 0;
            lastDevices.streams[i].period = 0;
            lastDevices.streams[i].buffer = 0;
            lastDevices.streams[i].mmap = false;
        }
        context.deviceHealth.publish(lastDevices);
        for (auto &node : context.nodeChannelLevels)
            for (auto &level : node) level.store(0, std::memory_order_relaxed);
        audiox::adaptiveAudioBuffer.publishObserved(0, 0, 0);
        shared->control.~Mailbox();
        new (&shared->control) audiox::worker::Mailbox<Control>;
        shared->telemetry.~Mailbox();
        new (&shared->telemetry) audiox::worker::Mailbox<Telemetry>;
        shared->soundRead.store(0);
        shared->soundWrite.store(0);
        shared->scriptRead.store(0);
        shared->scriptWrite.store(0);
        shared->stopAll.store(0);
        shared->stopHeld.store(0);
        shared->stop.store(false);
        shared->ready.store(false);
        shared->heartbeat.store(0);
        shared->controlHeartbeat.store(0);
        lastXruns = 0;
        ready.store(false, std::memory_order_release);
    }

    bool spawn() {
        publishControl();
        posix_spawn_file_actions_t actions;
        int rc = posix_spawn_file_actions_init(&actions);
        if (rc) { errno = rc; return false; }
        rc = posix_spawn_file_actions_adddup2(&actions, fd, 3);
        if (!rc) rc = posix_spawn_file_actions_addclosefrom_np(&actions, 4);
        char parent[24];
        snprintf(parent, sizeof(parent), "%d", getpid());
        char *argv[] = {path.data(), const_cast<char *>("3"), parent, nullptr};
        if (!rc) rc = posix_spawn(&pid, path.c_str(), &actions, nullptr, argv, environ);
        posix_spawn_file_actions_destroy(&actions);
        if (rc) {
            pid = -1;
            printf("[AUDIO] [ERR] start audio worker %s: %s\n", path.c_str(), strerror(rc));
            errno = rc;
            return false;
        }
        publicPid.store(pid, std::memory_order_release);
        startedAt = audiox::healthMonotonicMs();
        audiox::audioHealth.workerRestarted(startedAt);
        printf("[AUDIO] [INFO] audio worker started pid=%d plugins=%s\n",
               pid, recovery.pluginsDisabled ? "disabled (crash recovery)" : "enabled");
        return true;
    }

    void terminate() {
        if (pid <= 0) return;
        shared->stop.store(true, std::memory_order_release);
        int status = 0;
        for (unsigned i = 0; i < 50; ++i) {
            const pid_t result = waitpid(pid, &status, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD)) {
                pid = -1;
                publicPid.store(-1, std::memory_order_release);
                return;
            }
            usleep(10000);
        }
        if (kill(pid, SIGKILL) < 0 && errno != ESRCH)
            printf("[AUDIO] [ERR] kill stalled audio worker: %s\n", strerror(errno));
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        pid = -1;
        publicPid.store(-1, std::memory_order_release);
    }

    void failed(uint64_t now, int status) {
        lastExit.store(status, std::memory_order_release);
        const bool previouslyDisabled = recovery.pluginsDisabled;
        recovery.failed(now);
        consecutiveFailures.store(recovery.failures, std::memory_order_release);
        disabled.store(recovery.pluginsDisabled, std::memory_order_release);
        restartCount.fetch_add(1, std::memory_order_release);
        printf("[AUDIO] [ERR] audio worker failed status=%d; restart in %llu ms (%u consecutive failures)\n",
               status, static_cast<unsigned long long>(recovery.retryAt - now), recovery.failures);
        if (!previouslyDisabled && recovery.pluginsDisabled)
            printf("[AUDIO] [ERR] repeated audio worker failures: ALL plugins bypassed; configuration retained\n");
        resetAfterExit();
    }
};

AudioWorkerSupervisor::AudioWorkerSupervisor(AudioContext &context, const char *path)
    : state_(new State(context, path)) {}

AudioWorkerSupervisor::~AudioWorkerSupervisor() {
    state_->running.store(false, std::memory_order_release);
    if (started_) pthread_join(thread_, nullptr);
    auto &ctx = state_->context;
    ctx.sfxSlotRefs = ctx.localSfxSlotRefs;
    ctx.bluetoothInputs = ctx.localBluetoothInputs;
    ctx.bluetoothOutputs = ctx.localBluetoothOutputs;
    if (state_->shared) munmap(state_->shared, sizeof(Shared));
    if (state_->fd >= 0) close(state_->fd);
    delete state_;
}

bool AudioWorkerSupervisor::start() {
    if (!state_->map()) {
        printf("[AUDIO] [ERR] create audio worker transport: %s\n", strerror(errno));
        return false;
    }
    state_->running.store(true, std::memory_order_release);
    const int rc = pthread_create(&thread_, nullptr, threadMain, state_);
    if (rc) {
        state_->running.store(false);
        printf("[AUDIO] [ERR] start audio supervisor: %s\n", strerror(rc));
        return false;
    }
    started_ = true;
    return true;
}

void *AudioWorkerSupervisor::threadMain(void *arg) {
    auto &state = *static_cast<State *>(arg);
    while (state.running.load(std::memory_order_acquire)) {
        const uint64_t now = audiox::healthMonotonicMs();
        if (state.restartRequested.exchange(false, std::memory_order_acq_rel)) {
            state.ready.store(false, std::memory_order_release);
            state.terminate();
            state.resetAfterExit();
            state.recovery.manual(state.enableRequested.exchange(false, std::memory_order_acq_rel));
            state.disabled.store(state.recovery.pluginsDisabled, std::memory_order_release);
            state.consecutiveFailures.store(0, std::memory_order_release);
            state.restartCount.fetch_add(1, std::memory_order_release);
        }
        if (state.pid <= 0 && now >= state.recovery.retryAt && !state.spawn())
            state.failed(now, -errno);
        if (state.pid > 0) {
            int status = 0;
            const pid_t result = waitpid(state.pid, &status, WNOHANG);
            const uint64_t heartbeat = state.shared->heartbeat.load(std::memory_order_acquire);
            const uint64_t control = state.shared->controlHeartbeat.load(std::memory_order_acquire);
            const bool hung = (heartbeat ? now > heartbeat + 3000 : now > state.startedAt + 30000) ||
                              (control ? now > control + 30000 : now > state.startedAt + 30000);
            if (result == state.pid || (result < 0 && errno != EINTR) || hung) {
                if (result == state.pid) {
                    state.pid = -1;
                    state.publicPid.store(-1, std::memory_order_release);
                } else {
                    printf("[AUDIO] [ERR] audio worker heartbeat timed out or wait failed\n");
                    state.terminate();
                    status = -ETIMEDOUT;
                }
                (void)state.readTelemetry();
                state.failed(now, status);
            } else {
                state.publishControl();
                state.publishSounds();
                if (!state.readTelemetry()) {
                    state.terminate();
                    state.failed(now, -EPROTO);
                    continue;
                }
                state.ready.store(state.shared->ready.load(std::memory_order_acquire) && heartbeat != 0 &&
                                      !state.restartRequested.load(std::memory_order_acquire),
                                  std::memory_order_release);
                if (state.ready.load(std::memory_order_relaxed)) {
                    state.recovery.stable(now, state.startedAt);
                    state.consecutiveFailures.store(state.recovery.failures, std::memory_order_release);
                }
            }
        }
        usleep(5000);
    }
    state.terminate();
    state.resetAfterExit();
    return nullptr;
}

int AudioWorkerSupervisor::restart(bool enablePlugins) {
    if (!started_) return RET_ERR;
    if (enablePlugins) state_->enableRequested.store(true, std::memory_order_release);
    state_->restartRequested.store(true, std::memory_order_release);
    state_->ready.store(false, std::memory_order_release);
    return RET_OK;
}

std::string AudioWorkerSupervisor::statusJson() const {
    char json[512];
    snprintf(json, sizeof(json),
             "{\"running\":%s,\"pid\":%d,\"plugins_disabled\":%s,\"restarts\":%u,"
             "\"consecutive_failures\":%u,\"last_exit_status\":%d}",
             state_->ready.load(std::memory_order_acquire) ? "true" : "false",
             state_->publicPid.load(std::memory_order_acquire),
             state_->disabled.load(std::memory_order_acquire) ? "true" : "false",
             state_->restartCount.load(std::memory_order_acquire),
             state_->consecutiveFailures.load(std::memory_order_acquire),
             state_->lastExit.load(std::memory_order_acquire));
    return json;
}

void AudioContext::stopAudioWorker() {
    if (!workerSupervisor) return;
    delete workerSupervisor;
    workerSupervisor = nullptr;
    processingThreadRun.store(0, std::memory_order_release);
    processingThreadStarted = 0;
}

int AudioContext::restartAudio(bool enablePlugins) {
    if (!workerSupervisor) {
        printf("[AUDIO] [WARN] cannot restart audio: supervisor is not running\n");
        return RET_ERR;
    }
    return workerSupervisor->restart(enablePlugins);
}

std::string AudioContext::audioWorkerStatusJson() const {
    return workerSupervisor ? workerSupervisor->statusJson()
        : "{\"running\":false,\"pid\":-1,\"plugins_disabled\":false,\"restarts\":0,\"consecutive_failures\":0,\"last_exit_status\":0}";
}

namespace audiox::worker {
namespace {

bool applyControl(AudioContext &ctx, const Control &control,
                  std::unordered_map<std::string, uint64_t> &pluginRevisions) {
    {
        std::lock_guard<std::mutex> devices(ctx.devicesMutex);
        ctx.devices.clear();
        for (unsigned i = 0; i < std::min(control.deviceCount, 64U); ++i)
            ctx.devices.emplace(control.devices[i].handle, control.devices[i]);
    }
    if (ctx.effectStatesSeq.load(std::memory_order_acquire) != control.effectSequence) {
        std::string error;
        if (!audiox::plugin::loadCatalog(error))
            printf("[PLUGIN] [WARN] worker catalog: %s\n", error.c_str());
        for (unsigned i = 0; i < control.effectCount; ++i) {
            const auto &effect = control.effects[i];
            const uint8_t type = audiox::effects::effectTypeFromString(effect.type);
            bool needsHost = false;
            {
                std::lock_guard<std::mutex> effects(ctx.effectsMutex);
                const auto found = ctx.effectStates.find(effect.id);
                needsHost = !control.pluginsDisabled && audiox::plugin::isPluginType(type) &&
                    (found == ctx.effectStates.end() || found->second.type != type ||
                     pluginRevisions[effect.id] != effect.revision);
            }
            std::string preparationError;
            auto host = needsHost ? ctx.preparePlugin(effect.id, type, &preparationError) : nullptr;
            std::lock_guard<std::mutex> effects(ctx.effectsMutex);
            auto &params = ctx.effectStates[effect.id];
            if (params.type != type || needsHost) {
                ctx.retirePlugin(effect.id);
                params.pluginHost = nullptr;
            }
            params.type = type;
            params.enabled = control.pluginsDisabled && audiox::plugin::isPluginType(type) ? 0 : effect.enabled;
            std::copy_n(effect.values, audiox::effects::EFFECT_PARAM_MAX, params.values);
            if (host) ctx.attachPlugin(effect.id, params, std::move(host));
            pluginRevisions[effect.id] = effect.revision;
            if (audiox::plugin::isPluginType(type)) {
                auto &runtime = ctx.pluginRuntimeStates[effect.id];
                runtime.revision = effect.revision;
                if (control.pluginsDisabled || needsHost) {
                    runtime.status = control.pluginsDisabled ? audiox::plugin::PluginHost::Status::Stopped :
                        params.pluginHost ? params.pluginHost->status() : audiox::plugin::PluginHost::Status::Fault;
                    snprintf(runtime.error, sizeof(runtime.error), "%s", control.pluginsDisabled ?
                        "All plugins bypassed after repeated audio failures; use System to retry" :
                        preparationError.c_str());
                }
            } else {
                ctx.pluginRuntimeStates.erase(effect.id);
            }
        }
        {
            std::lock_guard<std::mutex> effects(ctx.effectsMutex);
            for (auto it = ctx.effectStates.begin(); it != ctx.effectStates.end();) {
                bool exists = false;
                for (unsigned i = 0; i < control.effectCount; ++i)
                    exists |= it->first == control.effects[i].id;
                if (exists) { ++it; continue; }
                ctx.retirePlugin(it->first.c_str());
                pluginRevisions.erase(it->first);
                ctx.pluginRuntimeStates.erase(it->first);
                it = ctx.effectStates.erase(it);
            }
            ctx.effectStatesSeq.store(control.effectSequence, std::memory_order_release);
        }
    }
    {
        std::lock_guard<std::mutex> gains(ctx.gainsMutex);
        for (unsigned i = 0; i < control.graph.thingCount; ++i) {
            ctx.thingGains[control.graph.things[i].id] = control.gains[i];
            if (ctx.processingGraphGeneration.load(std::memory_order_acquire) == control.graph.generation)
                ctx.nodeGainAtomics[i].store(control.gains[i], std::memory_order_relaxed);
        }
    }
    ctx.gadgetVolume.captureGain.store(control.captureGain, std::memory_order_release);
    ctx.gadgetVolume.playbackGain.store(control.playbackGain, std::memory_order_release);
    const bool released = control.releaseGeneration &&
        ctx.processingGraphGeneration.load(std::memory_order_acquire) >= control.releaseGeneration;
    ctx.sceneMuted.store(control.muted && !released, std::memory_order_release);
    ctx.sceneReleaseGeneration.store(control.releaseGeneration, std::memory_order_release);
    ctx.sceneMuteEpoch.store(control.sceneEpoch, std::memory_order_release);
    audiox::audioStressTest.mirrorAudioControl(control.stressExpiryNs, control.stressWorkNs);
    if (ctx.routingGraphPublished.generation != control.graph.generation) {
        ctx.routingGraphPublished = control.graph;
        ctx.routingGraphMailbox.publish(control.graph);
    }
    return true;
}

void receiveSounds(AudioContext &ctx, Shared &shared, uint64_t revisions[AUDIO_SFX_SLOT_COUNT]) {
    unsigned read = shared.soundRead.load(std::memory_order_relaxed);
    const unsigned end = shared.soundWrite.load(std::memory_order_acquire);
    unsigned write = ctx.sfxQueueWrite.load(std::memory_order_relaxed);
    while (read != end && write - ctx.sfxQueueRead.load(std::memory_order_acquire) < AUDIO_SFX_QUEUE_CAP) {
        const auto &sound = shared.sounds[read % AUDIO_SFX_QUEUE_CAP];
        const unsigned slot = sound.trigger.slotIndex;
        if (revisions[slot] != sound.revision) {
            auto &clip = ctx.sfxSlots[slot];
            clip = {};
            snprintf(clip.name, sizeof(clip.name), "%s", sound.name);
            clip.loaded = 1;
            clip.channels = sound.channels;
            clip.frames = sound.frames;
            clip.sampleRate = sound.sampleRate;
            clip.sampleCount = sound.sampleCount;
            clip.pcm = shared.pcm[slot];
            revisions[slot] = sound.revision;
        }
        ctx.sfxQueue[write % AUDIO_SFX_QUEUE_CAP] = sound.trigger;
        ctx.sfxQueueWrite.store(++write, std::memory_order_release);
        ++read;
    }
    shared.soundRead.store(read, std::memory_order_release);
    if (shared.stopAll.exchange(0, std::memory_order_acq_rel)) ctx.stopAllSfx();
    if (shared.stopHeld.exchange(0, std::memory_order_acq_rel)) ctx.stopHeldSfx();
}

void publishTelemetry(AudioContext &ctx, Shared &shared) {
    Telemetry data;
    for (const auto &entry : ctx.pluginRuntimeStates) {
        if (data.pluginCount >= AUDIO_GRAPH_MAX_THINGS) break;
        auto &plugin = data.plugins[data.pluginCount++];
        snprintf(plugin.id, sizeof(plugin.id), "%s", entry.first.c_str());
        plugin.runtime = entry.second;
    }
    data.devices = ctx.deviceHealth.read();
    for (unsigned n = 0; n < AUDIO_GRAPH_MAX_THINGS; ++n)
        for (unsigned ch = 0; ch < 16; ++ch)
            data.meters[n][ch] = ctx.nodeChannelLevels[n][ch].load(std::memory_order_relaxed);
    data.graphGeneration = ctx.processingGraphGeneration.load(std::memory_order_acquire);
    data.pluginSequence = ctx.pluginObservedSeq.load(std::memory_order_acquire);
    data.xruns = audiox::audioHealth.total();
    data.bufferRuntime = audiox::adaptiveAudioBuffer.instanceId();
    data.bufferLevel = audiox::adaptiveAudioBuffer.level();
    data.activeStreams = audiox::adaptiveAudioBuffer.activeStreams();
    data.largestPeriod = audiox::adaptiveAudioBuffer.largestPeriodFrames();
    data.largestBuffer = audiox::adaptiveAudioBuffer.largestBufferFrames();
    data.bufferChange = audiox::adaptiveAudioBuffer.changeId();
    data.bufferIncrease = audiox::adaptiveAudioBuffer.increaseId();
    data.downshiftDelay = audiox::adaptiveAudioBuffer.downshiftDelayMs();
    data.downshiftFailures = audiox::adaptiveAudioBuffer.downshiftFailures();
    data.processedSceneEpoch = ctx.processedSceneEpoch.load(std::memory_order_acquire);
    data.silent = ctx.sceneSilent.load(std::memory_order_acquire);
    data.releasedSceneEpoch = ctx.releasedSceneEpoch.load(std::memory_order_acquire);
    {
        std::lock_guard<std::mutex> playing(ctx.sfxPlayingMutex);
        data.playingCount = ctx.sfxPlayingCount;
        memcpy(data.playing, ctx.sfxPlayingBasenames, sizeof(data.playing));
    }
    shared.telemetry.publish(data);
    shared.heartbeat.store(ctx.processingProgressAtMs.load(std::memory_order_acquire), std::memory_order_release);
    const unsigned dropped = ctx.scriptSoundDropped.exchange(0, std::memory_order_acq_rel);
    if (dropped) shared.scriptDropped.fetch_add(dropped, std::memory_order_release);
    AudioContext::ScriptSoundEvent event;
    while (ctx.popScriptSoundEvent(&event)) {
        const unsigned write = shared.scriptWrite.load(std::memory_order_relaxed);
        if (write - shared.scriptRead.load(std::memory_order_acquire) >= 128) {
            shared.scriptDropped.fetch_add(1, std::memory_order_release);
            continue;
        }
        shared.scriptSounds[write % 128] = event;
        shared.scriptWrite.store(write + 1, std::memory_order_release);
    }
}
} // namespace

int run(int fd, int parentPid) {
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parentPid) {
        fprintf(stderr, "[AUDIO] [ERR] audio worker parent unavailable\n");
        return 1;
    }
    struct stat status{};
    if (fstat(fd, &status) < 0 || status.st_size != sizeof(Shared)) {
        fprintf(stderr, "[AUDIO] [ERR] invalid audio worker shared-memory size\n");
        return 1;
    }
    void *memory = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (memory == MAP_FAILED) {
        fprintf(stderr, "[AUDIO] [ERR] map audio worker transport: %s\n", strerror(errno));
        return 1;
    }
    auto &shared = *static_cast<Shared *>(memory);
    if (shared.magic != kMagic || shared.version != kVersion || shared.size != sizeof(Shared)) {
        fprintf(stderr, "[AUDIO] [ERR] audio worker protocol mismatch\n");
        munmap(memory, sizeof(Shared));
        return 1;
    }
    int result = 0;
    {
        audiox::plugin::setPluginExecutionEnabled(true);
        AudioContext ctx(nullptr, false);
        ctx.workerProcess = true;
        ctx.effectStatesSeq.store(0, std::memory_order_relaxed);
        ctx.sfxSlotRefs = shared.references;
        ctx.bluetoothInputs = shared.bluetoothInputs;
        ctx.bluetoothOutputs = shared.bluetoothOutputs;
        for (unsigned i = 0; i < AUDIO_BT_MAX_LINKS; ++i) {
            auto &input = ctx.bluetoothInputs[i];
            input.read.store(input.write.load(std::memory_order_acquire), std::memory_order_release);
            input.seenGeneration = input.generation.load(std::memory_order_acquire);
            input.primed = false;
            input.readFrac = input.filteredFill = input.rateIntegral = 0;
        }
        Control control;
        std::unordered_map<std::string, uint64_t> pluginRevisions;
        if (!shared.control.consume(control)) {
            fprintf(stderr, "[AUDIO] [ERR] audio worker started without configuration\n");
            result = 1;
        } else {
            applyControl(ctx, control, pluginRevisions);
            if (ctx.startProcessingThread() == RET_ERR) {
                result = 1;
            } else {
                shared.ready.store(true, std::memory_order_release);
                uint64_t revisions[AUDIO_SFX_SLOT_COUNT]{};
                while (!shared.stop.load(std::memory_order_acquire) && getppid() == parentPid) {
                    shared.controlHeartbeat.store(audiox::healthMonotonicMs(), std::memory_order_release);
                    if (shared.control.consume(control)) applyControl(ctx, control, pluginRevisions);
                    receiveSounds(ctx, shared, revisions);
                    ctx.pollPlugins();
                    publishTelemetry(ctx, shared);
                    usleep(5000);
                }
                ctx.processingThreadRun.store(0, std::memory_order_release);
                pthread_join(ctx.processingThread, nullptr);
                ctx.processingThreadStarted = 0;
            }
        }
        for (auto &clip : ctx.sfxSlots) clip.pcm = nullptr;
    }
    munmap(memory, sizeof(Shared));
    return result;
}
} // namespace audiox::worker
