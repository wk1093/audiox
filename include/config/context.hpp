#pragma once

#include <cstddef>
#include <cstdint>
#include "defs.hpp"
#include "../context.hpp"

#define CONFIG_REAL_FILE_PATH ROOT_MOUNT_POINT "/config.txt"
#define CONFIG_STAGING_FILE_PATH ROOT_MOUNT_POINT "/config.staging.txt"
#define ROUTING_REAL_FILE_PATH ROOT_MOUNT_POINT "/routing.txt"
#define MIDI_MAP_REAL_FILE_PATH ROOT_MOUNT_POINT "/midi_map.txt"
#define VOLUMES_FILE_PATH ROOT_MOUNT_POINT "/volumes.txt"
#define SFX_ROOT_DIR ROOT_MOUNT_POINT "/sfx"

#define MIDI_MAPPINGS_MAX 64
#define MIDI_SFX_PATH_MAX 160
#define MIDI_SOUND_LIGHTS_MAX MIDI_MAPPINGS_MAX
#define MIDI_SOUND_MODES_MAX MIDI_MAPPINGS_MAX
#define MIDI_ACTION_MAPPINGS_MAX MIDI_MAPPINGS_MAX
#define MIDI_CC_VOLUME_MAPPINGS_MAX 16
#define VOLUME_ENTRIES_MAX 64

struct VolumeEntry {
    char thingId[64];
    float gain; // 0.0 - 1.0
};

struct MidiCcVolumeMapping {
    uint8_t cc;
    char thingId[64];
};

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
    uint8_t soundboardMode;
};

enum SoundboardMode : uint8_t {
    SOUNDBOARD_MODE_PLAY = 0,
    SOUNDBOARD_MODE_HOLD = 1,
};

enum MidiActionType : uint8_t {
    MIDI_ACTION_NONE = 0,
    MIDI_ACTION_STOP_ALL = 1,
    MIDI_ACTION_SAMPLER_TOGGLE = 2,
};

const char *soundboardModeToString(uint8_t mode);
uint8_t soundboardModeFromString(const char *value);


// Global MIDI lighting config: one velocity per state, one channel for all output.
// "mapped"  = button is assigned to a sound but not active
// "playing" = clip is playing in non-hold mode but button is no longer held
// "stop_all" = button assigned to stop-all action
struct MidiLightGlobal {
    uint8_t channel;    // MIDI channel 0-15
    uint8_t mappedVel;  // note-on velocity for mapped/idle state (0 = off)
    uint8_t playingVel; // note-on velocity for playing state
    uint8_t stopAllVel; // note-on velocity for stop-all action mapping
};

// Per-sound lighting override.  If present, these velocities replace global values
// for that specific sound.  Fields marked has* = 0 fall back to global.
struct MidiSoundLight {
    char sfxPath[MIDI_SFX_PATH_MAX]; // basename, e.g. "kick.wav"
    uint8_t hasMapped;
    uint8_t hasPlaying;
    uint8_t mappedVel;
    uint8_t playingVel;
};

struct MidiSoundMode {
    char sfxPath[MIDI_SFX_PATH_MAX]; // basename, e.g. "kick.wav"
    uint8_t mode;                    // SoundboardMode
};

struct MidiActionMapping {
    uint8_t note;
    uint8_t action; // MidiActionType
};

// Contents of midi_map.txt: note→sfx mappings plus lighting config.
struct MidiMapData {
    uint32_t mappingCount;
    struct MidiMapping {
        uint8_t note;
        char sfxPath[MIDI_SFX_PATH_MAX];
    } mappings[MIDI_MAPPINGS_MAX];
    MidiLightGlobal globalLight;
    uint32_t soundLightCount;
    MidiSoundLight soundLights[MIDI_SOUND_LIGHTS_MAX];
    uint32_t soundModeCount;
    MidiSoundMode soundModes[MIDI_SOUND_MODES_MAX];
    uint32_t actionMappingCount;
    MidiActionMapping actionMappings[MIDI_ACTION_MAPPINGS_MAX];
    uint8_t samplerKeyboardEnabled;
    uint8_t samplerKeyboardChannel;
    uint8_t samplerRootNote;
    uint32_t ccVolumeMappingCount;
    MidiCcVolumeMapping ccVolumeMappings[MIDI_CC_VOLUME_MAPPINGS_MAX];
};

struct ConfigStore {
    // For interacting with the config files
    Audiox *app;

    ConfigStore(Audiox *context);
    ConfigData readConfigFile() const;
    int readStagingConfigFile(ConfigData *out) const;
    int writeConfigFile(ConfigData *cfg);

    // midi_map.txt: note→sfx mappings + lighting config (separate from config.txt)
    MidiMapData readMidiMapFile() const;
    int writeMidiMapFile(MidiMapData *data);

    // volumes.txt: per-thing output gain values
    int readVolumesFile(VolumeEntry *out, uint32_t *count_out, uint32_t cap) const;
    int writeVolumesFile(const VolumeEntry *entries, uint32_t count);

    RouterConfig router() const;

};
