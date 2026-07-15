#pragma once

#include "defs.hpp"
#include "../context.hpp"

#include <cstdint>
#include <cstddef>
#include <pthread.h>
#include <atomic>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

#define AUDIO_GRAPH_MAX_THINGS 64
#define AUDIO_GRAPH_MAX_EDGES 128
#define AUDIO_SFX_MAX_FRAMES (SAMPLE_RATE * 8)

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
    uint32_t frames;
    uint32_t sampleRate;
    uint8_t channels;
    int16_t pcm[AUDIO_SFX_MAX_FRAMES * 2];
};

struct AudioContext {
    Audiox *app;
    AudioRegistry registry;

    pthread_t processingThread;
    int processingThreadStarted;
    int processingThreadRun;

    uint64_t nextHotplugScanMs;
    AudioHandle nextHandle;
    uint32_t deviceGeneration;
    std::atomic<uint32_t> pendingSfxTriggers;
    AudioSfxClipSlot sfxSlots[2];
    std::atomic<uint32_t> sfxActiveSlot;

    std::unordered_map<std::string, AudioHandle> pathToHandle;
    std::unordered_map<AudioHandle, AudioDeviceInfo> devices;
    mutable std::mutex devicesMutex;
    AudioGraphState routingGraph;
    AudioGraphState routingGraphPublished;
    std::atomic<uint32_t> routingGraphSeq;
    mutable std::mutex routingGraphMutex;
    std::atomic<float> nodeChannelLevels[AUDIO_GRAPH_MAX_THINGS][16];

    AudioContext(Audiox *context);
    ~AudioContext();

    int setupThreads() WARN_UNUSED;
    int triggerSfx(const char *sfxPath) WARN_UNUSED;

    int forceRescan() WARN_UNUSED;
    size_t copyDeviceInfos(AudioDeviceInfo *out, size_t cap) const;
    int buildDevicesJson(char *out, size_t out_sz) const WARN_UNUSED;
    size_t copyRoutingThings(AudioGraphThingInfo *out, size_t cap) const;
    int reloadRoutingGraph() WARN_UNUSED;
    int buildRoutingGraphJson(char *out, size_t out_sz) const WARN_UNUSED;

    void poll();

    int rescanDevices();
    float getChannelLevel(AudioHandle handle, int channelIndex, bool isCapture) const;
};