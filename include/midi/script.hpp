#pragma once

#include <cstddef>
#include <cstdint>

inline constexpr size_t MIDI_SCRIPT_MAX = 16385;
inline constexpr size_t MIDI_SCRIPT_STORAGE_MAX = MIDI_SCRIPT_MAX + 1024;
inline constexpr unsigned MIDI_SCRIPT_ACTIONS_MAX = 128;
inline constexpr unsigned MIDI_SCRIPT_HOOKS_MAX = 32;
inline constexpr size_t MIDI_SCRIPT_MEMORY_MAX = 1024 * 1024;
inline constexpr unsigned MIDI_SCRIPT_INSTRUCTIONS_MAX = 100000;
inline constexpr unsigned MIDI_SCRIPT_STATE_ENTRIES_MAX = 32;
inline constexpr size_t MIDI_SCRIPT_STATE_KEY_MAX = 31;
inline constexpr size_t MIDI_SCRIPT_STATE_STRING_MAX = 127;
inline constexpr unsigned MIDI_SCRIPT_TYPE_CHARS_MAX = 1024;
inline constexpr size_t MIDI_SCRIPT_CONFIG_READ_MAX = 65536;
inline constexpr size_t MIDI_SCRIPT_CONFIG_WRITE_MAX = 4096;
inline constexpr size_t MIDI_SCRIPT_CONFIG_PLAN_MAX = 8192;

struct ConfigStore;
enum class MidiScriptEventKind : uint8_t {
    NoteOn, NoteOff, ControlChange, SoundStart, SoundFinish, DeviceConnect, DeviceDisconnect, Run,
    Boot, Timer, ConfigChange, Ui, BluetoothChange
};
inline constexpr uint16_t MIDI_SCRIPT_EVENTS_MASK = 8191;
inline constexpr uint16_t MIDI_SCRIPT_LEGACY_EVENTS_MASK = 127;
const char *midiScriptEventName(MidiScriptEventKind kind);

enum class MidiScriptKind { Tap, Wait, Light, Press, Release, Type, Log, SoundPlay, SoundStopAll, Midi,
                            EffectEnable, EffectToggle, EffectSet, VolumeSet, VolumeAdjust, ConfigWrite,
                            StorageWrite, StorageRemove, ScriptConfigSet, SystemRestart, BluetoothScan,
                            BluetoothPair, BluetoothConnect, BluetoothDisconnect, BluetoothUnpair,
                            BluetoothAutoConnect, SettingsSet };

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
    MidiScriptStateEntry setting;
};

struct MidiScriptPlan {
    unsigned count;
    MidiScriptAction actions[MIDI_SCRIPT_ACTIONS_MAX];
    unsigned configBytes;
    char configText[MIDI_SCRIPT_CONFIG_PLAN_MAX];
};

struct MidiScriptState {
    MidiScriptStateEntry entries[MIDI_SCRIPT_STATE_ENTRIES_MAX];
    unsigned count;
};

enum class MidiScriptUiKind { Boolean, Number, String, Button };
inline constexpr unsigned MIDI_SCRIPT_UI_MAX = 16;
struct MidiScriptUiControl {
    char key[32];
    char label[64];
    MidiScriptUiKind kind;
    MidiScriptStateEntry defaultValue;
    bool hasMin;
    bool hasMax;
    double min;
    double max;
};
struct MidiScriptUi {
    MidiScriptUiControl controls[MIDI_SCRIPT_UI_MAX];
    unsigned count;
};
bool midiScriptUiValueValid(const MidiScriptUiControl &control, const MidiScriptStateEntry &entry);

inline bool midiScriptConfigTextValid(const char *text, size_t length) {
    if (!text || length > MIDI_SCRIPT_CONFIG_WRITE_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') return false;
    }
    return true;
}

struct MidiScriptEvent {
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    bool pressed{true};
    uint64_t monotonicMs{0};
    int64_t epochSeconds{0};
    bool fromWeb{false};
    MidiScriptEventKind kind{MidiScriptEventKind::NoteOn};
    char sound[160]{};
    char device[256]{};
    char key[32]{};
    MidiScriptStateEntry value{};
    bool hasValue{true};
};

// Evaluate text once into an isolated, bounded plan. No actions are executed.
// Device calls only append validated actions; libraries are a bounded allowlist.
// millis()/epoch() return the supplied trigger snapshots, never live clocks.
// Source is NUL-terminated; callers accepting byte strings must reject embedded NULs.
bool midiEvaluateScript(const char *source, const MidiScriptEvent &event,
                        MidiScriptState &state, MidiScriptPlan &plan,
                        char *error, size_t errorSize, ConfigStore *config = nullptr,
                        bool configWriteAllowed = false, bool hookMode = false,
                        uint16_t expectedHookMask = 0, uint32_t scriptId = 0);

// Execute registration only in an isolated sandbox, without state, event, or device I/O.
// The returned mask is authoritative for callback scripts; callbacks are not invoked.
bool midiInspectHooks(const char *source, uint16_t &mask, char *error, size_t errorSize);
bool midiInspectUi(const char *source, MidiScriptUi &ui, char *error, size_t errorSize);

// Parse text without running it. The same source, memory, and instruction limits apply.
bool midiValidateScript(const char *source, char *error, size_t errorSize);

// Backwards-compatible stateless compiler wrapper.
bool midiCompileScript(const char *source, MidiScriptPlan &plan, char *error, size_t errorSize);
