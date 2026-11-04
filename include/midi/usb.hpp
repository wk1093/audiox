#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>
#include "scripting/script.hpp"
#include "scripting/keyboard.hpp"

inline constexpr const char *AUDIOX_MIDI_CARD_ID = "AudioXMIDI";
bool midiGadgetNode(const char *node);

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
    // Duplicate of keyboardFd; both report collections use the same HID endpoint.
    int mediaFd{-1};
    uint64_t nextProbeMs{0};
    uint64_t releaseMs{0};
    uint8_t pending[4096]{};
    size_t pendingSize{0};
    uint8_t chords[SCRIPTS_MAX][8]{};
    bool chordHolds[SCRIPTS_MAX]{};
    bool chordMedia[SCRIPTS_MAX]{};
    unsigned chordCount{0};
    bool keyDown{false};
    bool keyHeld{false};
    bool currentMedia{false};
    bool mediaNeedsRelease{false};
    bool keyboardNeedsRelease{false};
    uint64_t sessionGeneration{0};
    uint64_t keyboardPressMs{0};
    uint8_t mode{0};
    bool keyboardEnabled{false};
    uint8_t notePolicy[16][128]{};

    ~MidiUsbOutput();
    void poll(uint64_t now, uint8_t passthrough, bool keyboard);
    void forward(const uint8_t *bytes, size_t count, bool mapped);
    bool trigger(const char *keys);
    bool queueKeyboardReport(const uint8_t (&report)[8], bool hold = false);
    bool queueMediaReport(uint16_t usage, bool hold = false);
    bool press(const char *keys);
    bool release();
    void cancelChords();
    void closeDevices();
};
