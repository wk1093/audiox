#pragma once

#include "defs.hpp"
#include "../context.hpp"
#include <cstdint>
#include <unordered_map>
#include <mutex>
// an abstract handle to a routable object with any number of in/out channels
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

}; 
// a graph of routable things and their connections
// graphs can be nested, but this probably won't be implemented/used for a while.
// eventually the user will be able to create their own graphs as their own "effect pipeline"
// and duplicate occurrences of the same effect in different graphs, etc.
struct AudioRouteGraph : public AudioThing {}; 
// physical device like usb gadget or plugged in usb audio, and eventually
// The custom PCB with a PCM3168 6x8 interface.
struct AudioDevice : public AudioThing {}; 
// Special class for the USB gadget, which is a special case because it's number of channels can be changed
struct AudioUsbGadget : public AudioDevice {
    // Calling this method will invalidate this class because the device has to reconnect
    // our hotplug detection needs to be very smart and handle invalidation
    // maybe we should use a sort of "handle" system where the processing pipeline just uses a handle (integer) and we have a system to prevent
    // bad writes to invalidated handles, and we can just reassign the handle to a new object when the device reconnects.
    int reconfigure(uint32_t playbackChannels, uint32_t captureChannels, uint32_t sampleRate, uint32_t sampleSize) WARN_UNUSED;
};

// An effect or processing node
struct AudioEffect : public AudioThing {}; 

typedef uint32_t AudioHandle;

struct AudioRegistry {
    std::unordered_map<AudioHandle, AudioThing*> objects;
    std::mutex registryMutex;

    AudioThing* get(AudioHandle handle) {
        std::lock_guard<std::mutex> lock(registryMutex);
        auto it = objects.find(handle);
        if (it != objects.end()) {
            return it->second;
        }
        return nullptr;
    }

    void add(AudioHandle handle, AudioThing* thing) {
        std::lock_guard<std::mutex> lock(registryMutex);
        objects[handle] = thing;
    }

    void remove(AudioHandle handle) {
        std::lock_guard<std::mutex> lock(registryMutex);
        objects.erase(handle);
    }
};

struct AudioContext {
    Audiox *app;

    AudioContext(Audiox *context);

    int setupThreads() WARN_UNUSED;
    int triggerSfx(const char *sfxPath) WARN_UNUSED;
    void poll();
};