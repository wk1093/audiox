#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "midi/script.hpp"

inline constexpr const char *AUDIOX_MIDI_CARD_ID = "AudioXMIDI";
inline constexpr unsigned MIDI_CHORDS_MAX = 16;

struct MidiChord {
    uint8_t channel;
    uint8_t note;
    char keys[64];
    uint8_t idleVelocity;
    uint8_t pressedVelocity;
    char script[MIDI_SCRIPT_MAX];
    uint32_t id{0};
    char name[64]{};
    bool midiEnabled{true};
};

inline bool midiMacroEqual(const MidiChord &a, const MidiChord &b) {
    return a.id == b.id && a.midiEnabled == b.midiEnabled && a.channel == b.channel && a.note == b.note &&
        a.idleVelocity == b.idleVelocity && a.pressedVelocity == b.pressedVelocity &&
        !std::strncmp(a.name, b.name, sizeof(a.name)) &&
        !std::strncmp(a.keys, b.keys, sizeof(a.keys)) &&
        !std::strncmp(a.script, b.script, sizeof(a.script));
}

bool midiChordReport(const char *keys, uint8_t (&report)[8], bool allowModifiers = false);
bool midiAsciiReport(char character, uint8_t (&report)[8]);
bool midiGadgetNode(const char *node);
std::string midiHexEncode(const char *text);
bool midiHexDecode(const char *hex, char *text, size_t capacity);

struct MidiStream {
    uint8_t bytes[1024]{};
    size_t count{0};
    size_t needed{0};
    uint8_t running{0};
    bool sysex{false};

    template <typename Emit>
    void feed(uint8_t byte, Emit emit) {
        if (byte >= 0xf8) {
            emit(&byte, 1);
            return;
        }
        if (sysex) {
            if (byte == 0xf7) {
                bytes[count++] = byte;
                emit(bytes, count);
                sysex = false;
                count = 0;
                return;
            }
            if (!(byte & 0x80)) {
                bytes[count++] = byte;
                if (count == sizeof(bytes)) {
                    emit(bytes, count);
                    count = 0;
                }
                return;
            }
            sysex = false;
        }
        if (byte & 0x80) {
            count = needed = 0;
            if (byte >= 0xf0) running = 0;
            if (byte == 0xf0) {
                sysex = true;
                bytes[count++] = byte;
                return;
            }
            if (byte < 0xf0) {
                running = byte;
                needed = (byte & 0xe0) == 0xc0 ? 2 : 3;
            } else if (byte == 0xf1 || byte == 0xf3) needed = 2;
            else if (byte == 0xf2) needed = 3;
            else {
                if (byte == 0xf6 || byte == 0xf7) emit(&byte, 1);
                return;
            }
            bytes[count++] = byte;
            return;
        }
        if (!count && running) {
            bytes[count++] = running;
            needed = (running & 0xe0) == 0xc0 ? 2 : 3;
        }
        if (!needed) return;
        bytes[count++] = byte;
        if (count == needed) {
            emit(bytes, count);
            count = needed = 0;
        }
    }
};

struct MidiUsbOutput {
    MidiUsbOutput() = default;
    MidiUsbOutput(const MidiUsbOutput &) = delete;
    MidiUsbOutput &operator=(const MidiUsbOutput &) = delete;
    int midiFd{-1};
    int keyboardFd{-1};
    uint64_t nextProbeMs{0};
    uint64_t releaseMs{0};
    uint8_t pending[4096]{};
    size_t pendingSize{0};
    uint8_t chords[MIDI_CHORDS_MAX][8]{};
    bool chordHolds[MIDI_CHORDS_MAX]{};
    unsigned chordCount{0};
    bool keyDown{false};
    bool keyHeld{false};
    uint64_t sessionGeneration{0};
    uint64_t keyboardPressMs{0};
    uint8_t mode{0};
    bool keyboardEnabled{false};
    uint8_t notePolicy[16][128]{};

    ~MidiUsbOutput();
    void poll(uint64_t now, uint8_t passthrough, bool keyboard);
    void forward(const uint8_t *bytes, size_t count, bool mapped);
    void trigger(const char *keys);
    bool queueKeyboardReport(const uint8_t (&report)[8], bool hold = false);
    bool press(const char *keys);
    bool release();
    void cancelChords();
    void closeDevices();
};
