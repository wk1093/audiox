#pragma once

#include "audio/context.hpp"
#include "audio/worker.hpp"
#include <type_traits>

namespace audiox::worker {

static_assert(std::atomic<uint64_t>::is_always_lock_free);
static_assert(std::atomic<uint32_t>::is_always_lock_free);
static_assert(std::atomic<bool>::is_always_lock_free);

constexpr uint32_t kMagic = 0x41585731;
constexpr uint32_t kVersion = 1;

template <typename T> struct Mailbox {
    static_assert(std::is_trivially_copyable_v<T>);
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    T slots[3]{};
    std::atomic<unsigned> middle{1};
    unsigned back = 2, front = 0;
    void publish(const T &value) {
        slots[back] = value;
        back = middle.exchange(back | 4U, std::memory_order_acq_rel) & 3U;
    }
    bool consume(T &value) {
        if (!(middle.load(std::memory_order_acquire) & 4U)) return false;
        front = middle.exchange(front, std::memory_order_acq_rel) & 3U;
        value = slots[front];
        return true;
    }
};

struct Effect {
    char id[64]{};
    char type[24]{};
    uint8_t enabled{};
    uint64_t revision{};
    float values[audiox::effects::EFFECT_PARAM_MAX]{};
};

struct Control {
    AudioGraphState graph{};
    AudioDeviceInfo devices[64]{};
    uint32_t deviceCount{};
    Effect effects[AUDIO_GRAPH_MAX_THINGS]{};
    uint32_t effectCount{};
    uint32_t effectSequence{};
    float gains[AUDIO_GRAPH_MAX_THINGS]{};
    float captureGain{1}, playbackGain{1};
    bool muted{}, pluginsDisabled{};
    uint64_t sceneEpoch{};
    uint32_t releaseGeneration{};
    uint64_t stressExpiryNs{}, stressWorkNs{};
};

struct Telemetry {
    struct Plugin {
        char id[64]{};
        AudioContext::PluginRuntimeStatus runtime{};
    };
    Plugin plugins[AUDIO_GRAPH_MAX_THINGS]{};
    uint32_t pluginCount{};
    audiox::DeviceHealthSnapshot devices{};
    float meters[AUDIO_GRAPH_MAX_THINGS][16]{};
    uint32_t graphGeneration{}, pluginSequence{};
    uint64_t xruns{};
    uint32_t bufferLevel{}, activeStreams{}, largestPeriod{}, largestBuffer{};
    uint32_t downshiftDelay{}, downshiftFailures{};
    uint64_t bufferRuntime{}, bufferChange{}, bufferIncrease{};
    bool silent{};
    uint64_t processedSceneEpoch{}, releasedSceneEpoch{};
    uint32_t playingCount{};
    char playing[AUDIO_SFX_SLOT_COUNT][MIDI_SFX_PATH_MAX]{};
};

struct Sound {
    AudioSfxTriggerEvent trigger{};
    uint64_t revision{};
    char name[MIDI_SFX_PATH_MAX]{};
    uint32_t frames{}, sampleRate{}, sampleCount{};
    uint8_t channels{};
};

struct Shared {
    uint32_t magic{kMagic}, version{kVersion};
    uint64_t size{sizeof(Shared)};
    std::atomic<bool> stop{false}, ready{false};
    std::atomic<uint64_t> heartbeat{0}, controlHeartbeat{0};
    Mailbox<Control> control;
    Mailbox<Telemetry> telemetry;
    std::atomic<uint32_t> references[AUDIO_SFX_SLOT_COUNT]{};
    std::atomic<uint32_t> soundWrite{0}, soundRead{0};
    Sound sounds[AUDIO_SFX_QUEUE_CAP]{};
    std::atomic<uint32_t> stopAll{0}, stopHeld{0};
    std::atomic<uint32_t> scriptWrite{0}, scriptRead{0}, scriptDropped{0};
    AudioContext::ScriptSoundEvent scriptSounds[128]{};
    BluetoothInputStream bluetoothInputs[AUDIO_BT_MAX_LINKS];
    BluetoothOutputStream bluetoothOutputs[AUDIO_BT_MAX_LINKS];
    // Immutable while a queued trigger or playing voice holds a slot reference.
    int16_t pcm[AUDIO_SFX_SLOT_COUNT][AUDIO_SFX_MAX_FRAMES * 2U];
};

} // namespace audiox::worker
