#pragma once

#include <cstddef>
#include <cstdint>
#include "defs.hpp"
#include "../context.hpp"

#define CONFIG_REAL_FILE_PATH ROOT_MOUNT_POINT "/config.txt"
#define CONFIG_STAGING_FILE_PATH ROOT_MOUNT_POINT "/config.staging.txt"
#define ROUTING_REAL_FILE_PATH ROOT_MOUNT_POINT "/routing.txt"
#define SFX_ROOT_DIR ROOT_MOUNT_POINT "/sfx"

#define MIDI_MAPPINGS_MAX 64
#define MIDI_SFX_PATH_MAX 160

struct RouterConfig {
    RouterConfig();

    int getRouteCount() const;
    void getRoute(int index, char *out, size_t out_sz) const;
    void setRoute(int index, const char *route);
    void addRoute(const char *route);
    void removeRoute(int index);
};


struct ConfigData {
    uint32_t sampleRate;
    uint32_t playbackChannels;
    uint32_t captureChannels;
    uint32_t sampleSize;
    uint32_t mappingCount;
    struct MidiMapping {
        uint8_t note;
        char sfxPath[MIDI_SFX_PATH_MAX];
    } mappings[MIDI_MAPPINGS_MAX];
};

struct ConfigStore {
    // For interacting with the config files
    Audiox *app;

    ConfigStore(Audiox *context);
    ConfigData readConfigFile() const;
    int readStagingConfigFile(ConfigData *out) const;
    int writeConfigFile(ConfigData *cfg);
    int findMidiMapping(uint8_t note, ConfigData::MidiMapping *out) const;

    RouterConfig router() const; // no need for a setter, the router config manages itself, this is just a handy accessor.

};
