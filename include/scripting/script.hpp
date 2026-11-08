#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include "scripting/keyboard.hpp"

inline constexpr size_t SCRIPT_MAX = 16385;
inline constexpr size_t SCRIPT_STORAGE_MAX = SCRIPT_MAX + 1024;
inline constexpr unsigned SCRIPT_ACTIONS_MAX = 128;
inline constexpr unsigned SCRIPT_HOOKS_MAX = 32;
inline constexpr size_t SCRIPT_MEMORY_MAX = 1024 * 1024;
inline constexpr unsigned SCRIPT_INSTRUCTIONS_MAX = 100000;
inline constexpr unsigned SCRIPT_STATE_ENTRIES_MAX = 32;
inline constexpr size_t SCRIPT_STATE_KEY_MAX = 31;
inline constexpr size_t SCRIPT_STATE_STRING_MAX = 127;
inline constexpr unsigned SCRIPT_TYPE_CHARS_MAX = 1024;
inline constexpr size_t SCRIPT_CONFIG_READ_MAX = 65536;
inline constexpr size_t SCRIPT_CONFIG_WRITE_MAX = 4096;
inline constexpr size_t SCRIPT_CONFIG_PLAN_MAX = 8192;

struct ConfigStore;
struct ScriptMeters {
    struct Node {
        char id[64]{};
        uint8_t channels{};
        float levels[16]{};
    };
    Node nodes[64]{};
    unsigned count{};
};
inline constexpr unsigned SCRIPT_ACTIVITY_MAX = 8;
struct ScriptActivity {
    struct Device {
        char id[64]{};
        bool active{};
    };
    Device devices[SCRIPT_ACTIVITY_MAX]{};
    unsigned count{};
};
enum class ScriptEventKind : uint8_t {
    NoteOn, NoteOff, ControlChange, SoundStart, SoundFinish, DeviceConnect, DeviceDisconnect, Run,
    Boot, Timer, ConfigChange, Ui, BluetoothChange, BluetoothMediaKey, DeviceActive
};
inline constexpr unsigned SCRIPT_EVENT_COUNT = 15;
inline constexpr uint16_t SCRIPT_EVENTS_MASK = (1U << SCRIPT_EVENT_COUNT) - 1;
inline constexpr uint16_t SCRIPT_LEGACY_EVENTS_MASK = 127;
const char *scriptEventName(ScriptEventKind kind);

enum class ScriptKind { Tap, Wait, Light, Press, Release, Type, Log, SoundPlay, SoundStopAll, Midi,
                            EffectEnable, EffectToggle, EffectSet, VolumeSet, VolumeAdjust, ConfigWrite,
                            StorageWrite, StorageRemove, ScriptConfigSet, SystemRestart, BluetoothScan,
                            BluetoothPair, BluetoothConnect, BluetoothDisconnect, BluetoothUnpair,
                            BluetoothAutoConnect, SettingsSet };

enum class ScriptValueKind { Boolean, Number, String };

struct ScriptStateEntry {
    char key[32];
    ScriptValueKind kind;
    bool booleanValue;
    double numberValue;
    char stringValue[128];
    bool numberIsInteger;
    int64_t integerValue;
};

struct ScriptAction {
    ScriptKind kind;
    uint32_t value;
    char keys[64];
    char text[256];
    uint8_t midiBytes[3];
    uint8_t midiSize;
    double pitchRatio;
    char target[64];
    char parameter[64];
    double number;
    ScriptStateEntry setting;
};

struct ScriptPlan {
    unsigned count;
    ScriptAction actions[SCRIPT_ACTIONS_MAX];
    unsigned configBytes;
    char configText[SCRIPT_CONFIG_PLAN_MAX];
};

struct ScriptState {
    ScriptStateEntry entries[SCRIPT_STATE_ENTRIES_MAX];
    unsigned count;
};

enum class ScriptUiKind { Boolean, Number, String, Button };
inline constexpr unsigned SCRIPT_UI_MAX = 16;
struct ScriptUiControl {
    char key[32];
    char label[64];
    ScriptUiKind kind;
    ScriptStateEntry defaultValue;
    bool hasMin;
    bool hasMax;
    double min;
    double max;
};
struct ScriptUi {
    ScriptUiControl controls[SCRIPT_UI_MAX];
    unsigned count;
};
bool scriptUiValueValid(const ScriptUiControl &control, const ScriptStateEntry &entry);

inline bool scriptConfigTextValid(const char *text, size_t length) {
    if (!text || length > SCRIPT_CONFIG_WRITE_MAX) return false;
    for (size_t i = 0; i < length; ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (c < 32 && c != '\n' && c != '\r' && c != '\t') return false;
    }
    return true;
}

struct ScriptEvent {
    uint8_t channel;
    uint8_t note;
    uint8_t velocity;
    bool pressed{true};
    uint64_t monotonicMs{0};
    int64_t epochSeconds{0};
    bool fromWeb{false};
    ScriptEventKind kind{ScriptEventKind::NoteOn};
    char sound[160]{};
    char device[256]{};
    char key[32]{};
    ScriptStateEntry value{};
    bool hasValue{true};
    bool active{false};
};

// Evaluate text once into an isolated, bounded plan. No actions are executed.
// Device calls only append validated actions; libraries are a bounded allowlist.
// millis()/epoch() return the supplied trigger snapshots, never live clocks.
// Source is NUL-terminated; callers accepting byte strings must reject embedded NULs.
bool scriptEvaluate(const char *source, const ScriptEvent &event,
                        ScriptState &state, ScriptPlan &plan,
                        char *error, size_t errorSize, ConfigStore *config = nullptr,
                        bool configWriteAllowed = false, bool hookMode = false,
                        uint16_t expectedHookMask = 0, uint32_t scriptId = 0,
                        const ScriptMeters *meters = nullptr);

// Execute registration only in an isolated sandbox, without state, event, or device I/O.
// The returned mask is authoritative for callback scripts; callbacks are not invoked.
bool scriptInspectHooks(const char *source, uint16_t &mask, char *error, size_t errorSize,
                        ScriptActivity *activity = nullptr);
bool scriptInspectUi(const char *source, ScriptUi &ui, char *error, size_t errorSize);

// Parse text without running it. The same source, memory, and instruction limits apply.
bool scriptValidate(const char *source, char *error, size_t errorSize);

// Backwards-compatible stateless compiler wrapper.
bool scriptCompile(const char *source, ScriptPlan &plan, char *error, size_t errorSize);

inline constexpr unsigned SCRIPTS_MAX = 16;

struct ScriptDefinition {
    uint8_t channel;
    uint8_t note;
    char keys[64];
    uint8_t idleVelocity;
    uint8_t pressedVelocity;
    char script[SCRIPT_STORAGE_MAX];
    uint32_t id{0};
    char name[64]{};
    bool midiEnabled{true};
    uint16_t eventMask{1};
    bool configWriteAllowed{false};
    bool hookMode{false};
};

// Convert legacy sequences/chords without executing their code or granting permissions.
bool scriptMigrate(ScriptDefinition &script, char *error, size_t errorSize);

inline bool scriptMacroEqual(const ScriptDefinition &a, const ScriptDefinition &b) {
    return a.id == b.id && a.midiEnabled == b.midiEnabled && a.eventMask == b.eventMask &&
        a.configWriteAllowed == b.configWriteAllowed && a.hookMode == b.hookMode &&
        a.channel == b.channel && a.note == b.note &&
        a.idleVelocity == b.idleVelocity && a.pressedVelocity == b.pressedVelocity &&
        !std::strncmp(a.name, b.name, sizeof(a.name)) &&
        !std::strncmp(a.keys, b.keys, sizeof(a.keys)) &&
        !std::strncmp(a.script, b.script, sizeof(a.script));
}

inline bool scriptMacroTriggersOverlap(const ScriptDefinition &a, const ScriptDefinition &b) {
    return a.midiEnabled && b.midiEnabled && a.channel == b.channel && a.note == b.note &&
        (a.eventMask & b.eventMask & 7) != 0;
}

inline bool scriptNeedsDevicePermission(const char *source) {
    auto identifier = [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '_';
    };
    while (*source) {
        if (!identifier(*source)) { ++source; continue; }
        const char *start = source++;
        while (identifier(*source)) ++source;
        for (const char *name : {"config", "settings", "system", "bluetooth"})
            if (static_cast<size_t>(source - start) == strlen(name) &&
                !strncmp(start, name, static_cast<size_t>(source - start))) return true;
    }
    return false;
}
