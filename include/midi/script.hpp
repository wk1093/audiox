#pragma once

#include <cstddef>
#include <cstdint>

inline constexpr size_t MIDI_SCRIPT_MAX = 2048;
inline constexpr unsigned MIDI_SCRIPT_ACTIONS_MAX = 64;
inline constexpr unsigned MIDI_SCRIPT_STATE_ENTRIES_MAX = 32;
inline constexpr size_t MIDI_SCRIPT_STATE_KEY_MAX = 31;
inline constexpr size_t MIDI_SCRIPT_STATE_STRING_MAX = 127;
inline constexpr unsigned MIDI_SCRIPT_TYPE_CHARS_MAX = 256;

enum class MidiScriptKind { Tap, Wait, Light, Press, Release, Type, Log, SoundPlay, SoundStopAll, Midi,
                            EffectEnable, EffectToggle, EffectSet, VolumeSet, VolumeAdjust };

struct MidiScriptAction {
    MidiScriptKind kind;
    uint32_t value;
    char keys[64];
    char text[256];
    uint8_t midiBytes[3];
    uint8_t midiSize;
    double pitchRatio;
    char target[64];
    char parameter[64];
    double number;
};

struct MidiScriptPlan {
    unsigned count;
    MidiScriptAction actions[MIDI_SCRIPT_ACTIONS_MAX];
};

enum class MidiScriptValueKind { Boolean, Number, String };

struct MidiScriptStateEntry {
    char key[32];
    MidiScriptValueKind kind;
    bool booleanValue;
    double numberValue;
    char stringValue[128];
    bool numberIsInteger;
    int64_t integerValue;
};

struct MidiScriptState {
    MidiScriptStateEntry entries[MIDI_SCRIPT_STATE_ENTRIES_MAX];
    unsigned count;
};

struct MidiScriptEvent {
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    bool pressed{true};
    uint64_t monotonicMs{0};
    int64_t epochSeconds{0};
    bool fromWeb{false};
};

// Evaluate text once into an isolated, bounded plan. No actions are executed.
// Device calls only append validated actions; libraries are a bounded allowlist.
// millis()/epoch() return the supplied trigger snapshots, never live clocks.
// Source is NUL-terminated; callers accepting byte strings must reject embedded NULs.
bool midiEvaluateScript(const char *source, const MidiScriptEvent &event,
                        MidiScriptState &state, MidiScriptPlan &plan,
                        char *error, size_t errorSize);

// Parse text without running it. The same source, memory, and instruction limits apply.
bool midiValidateScript(const char *source, char *error, size_t errorSize);

// Backwards-compatible stateless compiler wrapper.
bool midiCompileScript(const char *source, MidiScriptPlan &plan, char *error, size_t errorSize);
