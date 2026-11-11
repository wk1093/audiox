#pragma once

#include "defs.hpp"
#include "config/context.hpp"
#include "../context.hpp"

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <pthread.h>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

#include "audio/effects/slot.hpp"
#include "audio/plugin_host.hpp"
#include "audio/gadget_volume.hpp"
#include "audio/bluetooth_stream.hpp"
#include "audio/device_health.hpp"
#include <vector>

#define AUDIO_GRAPH_MAX_THINGS 64
#define AUDIO_GRAPH_MAX_EDGES 128
#define AUDIO_SFX_MAX_FRAMES (SAMPLE_RATE * 8)
#define AUDIO_SFX_SLOT_COUNT 64
#define AUDIO_SFX_QUEUE_CAP 128

struct AudioGraphThingInfo {
    char id[64];
    char name[128];
    uint8_t inputs;
    uint8_t outputs;
};

struct AudioGraphEdgeInfo {
    char src[64];
    char dst[64];
    uint8_t srcChannel;
    uint8_t dstChannel;
};

struct AudioGraphState {
    uint32_t generation;
    uint32_t topologyGeneration;
    uint16_t thingCount;
    uint16_t edgeCount;
    uint16_t skippedEdgeCount;
    char status[160];
    AudioGraphThingInfo things[AUDIO_GRAPH_MAX_THINGS];
    AudioGraphEdgeInfo edges[AUDIO_GRAPH_MAX_EDGES];
};

// Serialized publishers and one audio-thread consumer exchange slot ownership.
// The consumer keeps its slot until the next exchange; neither side ever
// copies a slot owned by the other, even if several updates arrive per block.
struct AudioGraphMailbox {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    AudioGraphState slots[3]{};
    std::atomic<unsigned> middle{1};
    unsigned back = 2;
    unsigned front = 0;

    void publish(const AudioGraphState &graph) {
        slots[back] = graph;
        back = middle.exchange(back | 4U, std::memory_order_acq_rel) & 3U;
    }

    bool consume(AudioGraphState *out, bool includeCurrent = false) {
        if (!(middle.load(std::memory_order_acquire) & 4U)) {
            if (includeCurrent) *out = slots[front];
            return includeCurrent;
        }
        front = middle.exchange(front, std::memory_order_acq_rel) & 3U;
        *out = slots[front];
        return true;
    }
};

struct AudioThing {
    virtual uint32_t getInputChCount() const = 0;
    virtual uint32_t getOutputChCount() const = 0;
    virtual ~AudioThing() = default;

    virtual void process(float *in, float *out, uint32_t frames) = 0;
    virtual bool isActive() const = 0;
    virtual bool isDevice() const = 0;
    virtual bool isEffect() const = 0;
    virtual bool isGraph() const = 0;
    virtual bool isUsbGadget() const = 0;

    virtual const char *getDebugName() const = 0;
    virtual void setActive(bool active) = 0;
};

struct AudioRouteGraph : public AudioThing {};
struct AudioDevice : public AudioThing {};

struct AudioUsbGadget : public AudioDevice {
    int reconfigure(uint32_t playbackChannels, uint32_t captureChannels, uint32_t sampleRate, uint32_t sampleSize) WARN_UNUSED;
};

struct AudioEffect : public AudioThing {};

typedef uint32_t AudioHandle;

struct AudioDeviceInfo {
    AudioHandle handle;
    uint32_t generation;
    uint32_t cardIndex;
    uint32_t deviceIndex;
    uint8_t hasPlayback;
    uint8_t hasCapture;
    uint8_t isUsb;
    uint8_t isGadget;
    uint8_t playbackChannels;
    uint8_t captureChannels;
    char nodeName[64];
    char devPath[128];
    char displayName[128];
    // Stable USB identity when available, otherwise the ALSA card ID slug.
    char stableCardId[64];
    // Previous ALSA card ID slug, retained to migrate existing routes.
    char legacyCardId[64];
};

struct AudioRegistry {
    std::unordered_map<AudioHandle, std::unique_ptr<AudioThing>> objects;
    mutable std::mutex registryMutex;

    AudioThing* get(AudioHandle handle) {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto it = objects.find(handle);
        if (it != objects.end()) {
            return it->second.get();
        }
        return nullptr;
    }

    void add(AudioHandle handle, std::unique_ptr<AudioThing> thing) {
        std::lock_guard<std::mutex> lock(registryMutex);
        objects[handle] = std::move(thing);
    }

    void remove(AudioHandle handle) {
        std::lock_guard<std::mutex> lock(registryMutex);
        objects.erase(handle);
    }
};

struct AudioSfxClipSlot {
    uint8_t loaded;
    char name[MIDI_SFX_PATH_MAX];
    uint32_t frames;
    uint32_t sampleRate;
    uint8_t channels;
    uint32_t sampleCount;
    int16_t *pcm;
};

struct AudioSfxTriggerEvent {
    uint8_t slotIndex;
    uint8_t holdStart;
    float pitchRatio;
};

struct AudioEffectSlotState {
    char thingId[64];
    uint8_t enabled;
    uint8_t type;
    uint8_t paramCount;
    char paramNames[audiox::effects::EFFECT_PARAM_MAX][24];
    float paramValues[audiox::effects::EFFECT_PARAM_MAX];
};

class AudioWorkerSupervisor;

struct AudioContext {
    Audiox *app;
    audiox::DeviceHealthMailbox deviceHealth;
    AudioRegistry registry;

    pthread_t processingThread;
    int processingThreadStarted;
    std::atomic<int> processingThreadRun;
    std::atomic<uint64_t> processingProgressAtMs{0};
    std::atomic<uint32_t> processingGraphGeneration{0};
    AudioWorkerSupervisor *workerSupervisor{nullptr};
    bool workerProcess{false};
    mutable std::mutex workerStateMutex;
    audiox::usb::GadgetVolume gadgetVolume;

    std::atomic<uint64_t> nextHotplugScanMs;
    AudioHandle nextHandle;
    std::atomic<uint32_t> deviceGeneration;
    std::mutex rescanMutex;
    std::atomic<uint32_t> pendingSfxHoldStops;
    std::atomic<uint32_t> pendingSfxStopAll;
    AudioSfxClipSlot sfxSlots[AUDIO_SFX_SLOT_COUNT];
    uint64_t sfxBankRevision[AUDIO_SFX_SLOT_COUNT]{};
    std::atomic<uint32_t> localSfxSlotRefs[AUDIO_SFX_SLOT_COUNT]{};
    std::atomic<uint32_t> *sfxSlotRefs = localSfxSlotRefs;
    AudioSfxTriggerEvent sfxQueue[AUDIO_SFX_QUEUE_CAP];
    std::atomic<uint32_t> sfxQueueWrite;
    std::atomic<uint32_t> sfxQueueRead;
    std::atomic<uint32_t> sfxQueueDropped;
    mutable std::mutex sfxQueueMutex;
    mutable std::mutex sfxBankMutex;
    std::atomic<uint8_t> soundboardMode;

    // Soundboard play-state tracking for MIDI lighting.
    // sfxIsPlaying: set to 1 by the audio thread when a clip is audible, 0 when done.
    // sfxTriggerSeq: bumped (from the HTTP/trigger thread) each time a new clip starts.
    // sfxLastTriggeredBasename: basename of last triggered file, e.g. "kick.wav".
    std::atomic<int> sfxIsPlaying;
    std::atomic<uint32_t> sfxTriggerSeq;
    char sfxLastTriggeredBasename[256];
    mutable std::mutex sfxNameMutex;
    std::atomic<uint32_t> sfxPlayingSeq;
    uint32_t sfxPlayingCount;
    char sfxPlayingBasenames[AUDIO_SFX_SLOT_COUNT][MIDI_SFX_PATH_MAX];
    mutable std::mutex sfxPlayingMutex;
    struct ScriptSoundEvent {
        char sound[MIDI_SFX_PATH_MAX]{};
        bool started{false};
    };
    ScriptSoundEvent scriptSoundEvents[128]{};
    std::atomic<unsigned> scriptSoundWrite{0};
    std::atomic<unsigned> scriptSoundRead{0};
    std::atomic<unsigned> scriptSoundDropped{0};
    void publishScriptSoundEvent(const char *sound, bool started) {
        const unsigned writeIndex = scriptSoundWrite.load(std::memory_order_relaxed);
        if (writeIndex - scriptSoundRead.load(std::memory_order_acquire) >= 128) {
            scriptSoundDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        ScriptSoundEvent &event = scriptSoundEvents[writeIndex % 128];
        size_t length = strnlen(sound, sizeof(event.sound) - 1);
        memcpy(event.sound, sound, length);
        event.sound[length] = '\0';
        event.started = started;
        scriptSoundWrite.store(writeIndex + 1, std::memory_order_release);
    }
    bool popScriptSoundEvent(ScriptSoundEvent *event) {
        const unsigned readIndex = scriptSoundRead.load(std::memory_order_relaxed);
        if (!event || readIndex == scriptSoundWrite.load(std::memory_order_acquire)) return false;
        *event = scriptSoundEvents[readIndex % 128];
        scriptSoundRead.store(readIndex + 1, std::memory_order_release);
        return true;
    }

    std::unordered_map<std::string, AudioHandle> pathToHandle;
    std::unordered_map<AudioHandle, AudioDeviceInfo> devices;
    mutable std::mutex devicesMutex;
    AudioGraphState routingGraph;
    AudioGraphState routingGraphPublished;
    AudioGraphMailbox routingGraphMailbox;
    mutable std::mutex routingGraphMutex;
    std::atomic<float> nodeChannelLevels[AUDIO_GRAPH_MAX_THINGS][16];

    // One slot per Bluetooth link; a slot's peer selects which bt_<MAC> node it feeds.
    BluetoothInputStream localBluetoothInputs[AUDIO_BT_MAX_LINKS];
    BluetoothOutputStream localBluetoothOutputs[AUDIO_BT_MAX_LINKS];
    BluetoothInputStream *bluetoothInputs = localBluetoothInputs;
    BluetoothOutputStream *bluetoothOutputs = localBluetoothOutputs;

    // Per-thing output gain (0.0-1.0). Indexed by current routing snapshot node index.
    // Updated atomically so the audio thread reads without locking.
    std::atomic<float> nodeGainAtomics[AUDIO_GRAPH_MAX_THINGS];
    // Authoritative gain map by thing ID, persisted to volumes.txt.
    mutable std::mutex gainsMutex;
    std::unordered_map<std::string, float> thingGains;
    std::mutex sceneMutex;
    std::atomic<bool> sceneMuted{false};
    std::atomic<bool> sceneSilent{false};
    std::atomic<uint64_t> sceneMuteEpoch{0};
    std::atomic<uint64_t> processedSceneEpoch{0};
    std::atomic<uint64_t> releasedSceneEpoch{0};
    std::atomic<uint32_t> sceneReleaseGeneration{0};
    std::string activeSceneId;
    std::atomic<uint32_t> sceneRecallSeq{0};

    std::unordered_map<std::string, audiox::effects::SlotParams> effectStates;
    std::atomic<uint32_t> effectStatesSeq;
    uint32_t nextEffectId;
    mutable std::mutex effectsMutex;
    std::unordered_map<std::string, std::unique_ptr<audiox::plugin::PluginHost>> pluginHosts;
    struct PluginRuntimeStatus {
        uint64_t revision{};
        audiox::plugin::PluginHost::Status status{audiox::plugin::PluginHost::Status::Starting};
        char error[256]{};
    };
    std::unordered_map<std::string, PluginRuntimeStatus> pluginRuntimeStates;
    std::unordered_map<std::string, bool> pluginFaultReported;
    struct RetiredPlugin {
        uint32_t sequence;
        std::unique_ptr<audiox::plugin::PluginHost> host;
    };
    std::vector<RetiredPlugin> retiredPlugins;
    std::atomic<uint32_t> pluginObservedSeq{0};

    AudioContext(Audiox *context);
    AudioContext(Audiox *context, bool initializeState);
    ~AudioContext();

    int setupThreads() WARN_UNUSED;
    int startProcessingThread() WARN_UNUSED;
    int restartAudio(bool enablePlugins = false) WARN_UNUSED;
    void stopAudioWorker();
    std::string audioWorkerStatusJson() const;
    int triggerSfx(const char *sfxPath) WARN_UNUSED;
    int triggerSfxPitch(const char *sfxPath, float pitchRatio) WARN_UNUSED;
    int triggerPreloadedSfxPitch(const char *sfxPath, float pitchRatio) WARN_UNUSED;
    int startHeldSfx(const char *sfxPath) WARN_UNUSED;
    int reloadSfxBank() WARN_UNUSED;
    uint32_t loadedSfxCount() const;
    void stopHeldSfx();
    void stopAllSfx();
    void setSoundboardMode(uint8_t mode);
    uint8_t getSoundboardMode() const;

    int forceRescan() WARN_UNUSED;
    size_t copyDeviceInfos(AudioDeviceInfo *out, size_t cap) const;
    int buildDevicesJson(char *out, size_t out_sz) const WARN_UNUSED;
    size_t copyRoutingThings(AudioGraphThingInfo *out, size_t cap) const;
    int reloadRoutingGraph() WARN_UNUSED;
    int buildRoutingGraphJson(char *out, size_t out_sz) const WARN_UNUSED;
    int buildMetersJson(char *out, size_t outSize) const WARN_UNUSED;

    void poll();

    int rescanDevices();
    float getChannelLevel(AudioHandle handle, int channelIndex, bool isCapture) const;

    // Per-thing output volume (0.0-1.0). Thread-safe.
    void setThingGain(const char *thingId, float gain);
    float getThingGain(const char *thingId) const;
    // Snapshot thingGains into nodeGainAtomics using the given routing graph snapshot.
    void snapshotGainsForGraph(const AudioGraphState &graph);
    // Load gains from volumes.txt into thingGains.
    void loadVolumesFromConfig();

    void loadEffectsFromConfig();
    // Worker startup must not hold the mutex read by the audio thread.
    std::unique_ptr<audiox::plugin::PluginHost> preparePlugin(const char *id, uint8_t type,
                                                            std::string *error = nullptr);
    void attachPlugin(const char *id, audiox::effects::SlotParams &params,
                      std::unique_ptr<audiox::plugin::PluginHost> host);
    void retirePlugin(const char *id);
    void pollPlugins();
    bool pluginInUse(const char *type) const;
    int reloadPlugins(const char *type);
    int listEffects(AudioEffectSlotState *out, size_t cap) const;
    int getEffectParams(const char *thingId, audiox::effects::SlotParams *out) const;
    int setEffectType(const char *thingId, uint8_t type);
    int setEffectEnabled(const char *thingId, uint8_t enabled);

    float getGraphThingChannelLevel(const char *thingId, int channelIndex) const;
    int toggleEffectEnabled(const char *thingId);
    int persistEffectBypass(const char *effectId);
    int setEffectParam(const char *thingId, const char *paramName, float value);
    int setEffectParamNormalized(const char *thingId, const char *paramName, float normalized);
    int createEffect(const char *type, char *outId, size_t outIdSize);
    int deleteEffect(const char *thingId);
};