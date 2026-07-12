#pragma once

#include "defs.hpp"
#include "../context.hpp"

#include <cstdint>
#include <cstddef>
#include <pthread.h>
#include <memory>
#include <string>
#include <unordered_map>
#include <mutex>

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

struct AudioContext {
    Audiox *app;
    AudioRegistry registry;

    pthread_t processingThread;
    int processingThreadStarted;
    int processingThreadRun;

    uint64_t nextHotplugScanMs;
    AudioHandle nextHandle;
    uint32_t deviceGeneration;

    std::unordered_map<std::string, AudioHandle> pathToHandle;
    std::unordered_map<AudioHandle, AudioDeviceInfo> devices;
    mutable std::mutex devicesMutex;

    AudioContext(Audiox *context);
    ~AudioContext();

    int setupThreads() WARN_UNUSED;
    int triggerSfx(const char *sfxPath) WARN_UNUSED;

    int forceRescan() WARN_UNUSED;
    size_t copyDeviceInfos(AudioDeviceInfo *out, size_t cap) const;
    int buildDevicesJson(char *out, size_t out_sz) const WARN_UNUSED;

    void poll();

    int rescanDevices();
};