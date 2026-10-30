#include "midi/usb.hpp"
#include "defs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>

std::string midiHexEncode(const char *text) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string hex;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        hex += digits[*p >> 4];
        hex += digits[*p & 15];
    }
    return hex;
}

bool midiHexDecode(const char *hex, char *text, size_t capacity) {
    size_t length = strlen(hex);
    if (!capacity || length % 2 || length / 2 >= capacity) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < length; i += 2) {
        int hi = digit(hex[i]), lo = digit(hex[i + 1]);
        if (hi < 0 || lo < 0 || (hi == 0 && lo == 0)) return false;
        text[i / 2] = static_cast<char>((hi << 4) | lo);
    }
    text[length / 2] = '\0';
    return true;
}

bool midiChordReport(const char *keys, uint8_t (&report)[8], bool allowModifiers) {
    memset(report, 0, sizeof(report));
    if (!keys || !*keys || strlen(keys) >= 64) return false;
    char copy[64];
    strcpy(copy, keys);
    unsigned used = 2;
    char *start = copy;
    while (start) {
        char *end = strchr(start, '+');
        if (end) *end++ = '\0';
        for (char *p = start; *p; ++p) {
            if (*p >= 'a' && *p <= 'z') *p -= 'a' - 'A';
            if (!(*p >= 'A' && *p <= 'Z') && !(*p >= '0' && *p <= '9')) return false;
        }
        uint8_t key = 0;
        if (strcmp(start, "CTRL") == 0) report[0] |= 1;
        else if (strcmp(start, "SHIFT") == 0) report[0] |= 2;
        else if (strcmp(start, "ALT") == 0) report[0] |= 4;
        else if (strcmp(start, "GUI") == 0) report[0] |= 8;
        else {
            if (strlen(start) == 1 && *start >= 'A' && *start <= 'Z') key = 4 + *start - 'A';
            else if (strlen(start) == 1 && *start >= '1' && *start <= '9') key = 30 + *start - '1';
            else if (strcmp(start, "0") == 0) key = 39;
            else if (*start == 'F') {
                char *tail = nullptr;
                long f = strtol(start + 1, &tail, 10);
                if (*tail == '\0' && f >= 1 && f <= 24) key = f <= 12 ? 57 + f : 91 + f;
            }
            struct NamedKey { const char *name; uint8_t code; };
            static constexpr NamedKey names[] = {
                {"ENTER", 40}, {"ESC", 41}, {"BACKSPACE", 42}, {"TAB", 43}, {"SPACE", 44},
                {"DELETE", 76}, {"RIGHT", 79}, {"LEFT", 80}, {"DOWN", 81}, {"UP", 82},
                {"HOME", 74}, {"END", 77}, {"PAGEUP", 75}, {"PAGEDOWN", 78},
            };
            for (const auto &name : names) if (strcmp(start, name.name) == 0) key = name.code;
            if (!key || used == 8) return false;
            for (unsigned i = 2; i < used; ++i) if (report[i] == key) return false;
            report[used++] = key;
        }
        start = end;
    }
    return used > 2 || (allowModifiers && report[0]);
}

bool midiAsciiReport(char character, uint8_t (&report)[8]) {
    memset(report, 0, sizeof(report));
    if (character >= 'a' && character <= 'z') report[2] = 4 + character - 'a';
    else if (character >= 'A' && character <= 'Z') {
        report[0] = 2;
        report[2] = 4 + character - 'A';
    } else if (character >= '1' && character <= '9') report[2] = 30 + character - '1';
    else {
        struct AsciiKey { char plain; char shifted; uint8_t usage; };
        static constexpr AsciiKey keys[] = {
            {'0', ')', 39}, {'1', '!', 30}, {'2', '@', 31}, {'3', '#', 32},
            {'4', '$', 33}, {'5', '%', 34}, {'6', '^', 35}, {'7', '&', 36},
            {'8', '*', 37}, {'9', '(', 38}, {'-', '_', 45}, {'=', '+', 46},
            {'[', '{', 47}, {']', '}', 48}, {'\\', '|', 49}, {';', ':', 51},
            {'\'', '"', 52}, {'`', '~', 53}, {',', '<', 54}, {'.', '>', 55},
            {'/', '?', 56},
        };
        if (character == ' ') report[2] = 44;
        else if (character == '\n') report[2] = 40;
        else if (character == '\t') report[2] = 43;
        else for (const auto &key : keys) {
            if (character == key.plain || character == key.shifted) {
                report[0] = character == key.shifted ? 2 : 0;
                report[2] = key.usage;
                break;
            }
        }
    }
    return report[2] != 0;
}

bool midiGadgetNode(const char *node) {
    unsigned card;
    if (sscanf(node, "midiC%uD", &card) != 1) return false;
    char path[128], id[64]{};
    snprintf(path, sizeof(path), "/sys/class/sound/card%u/id", card);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    bool ok = fgets(id, sizeof(id), file) != nullptr;
    fclose(file);
    id[strcspn(id, "\r\n")] = '\0';
    return ok && strcmp(id, AUDIOX_MIDI_CARD_ID) == 0;
}

MidiUsbOutput::~MidiUsbOutput() { closeDevices(); }

void MidiUsbOutput::cancelChords() {
    chordCount = 0;
    keyHeld = false;
    releaseMs = 0;
}

static bool usbHostConfigured() {
    FILE *file = fopen("/sys/class/udc/" AUDIOX_USB_GADGET_UDC "/state", "r");
    if (!file) {
        if (errno != ENOENT) printf("[MIDI] [WARN] Cannot read USB host state: %s\n", strerror(errno));
        return false;
    }
    char state[32]{};
    bool ok = fgets(state, sizeof(state), file) != nullptr;
    if (!ok) printf("[MIDI] [WARN] Cannot read USB host state\n");
    fclose(file);
    return ok && strcmp(state, "configured\n") == 0;
}

void MidiUsbOutput::closeDevices() {
    if (midiFd >= 0 || keyboardFd >= 0) ++sessionGeneration;
    if (keyboardFd >= 0) {
        uint8_t released[8]{};
        ssize_t n = write(keyboardFd, released, sizeof(released));
        if (n != (ssize_t)sizeof(released)) {
            printf("[MIDI] [WARN] Could not release USB keyboard keys: %s\n", n < 0 ? strerror(errno) : "short report");
        }
        close(keyboardFd);
    }
    if (midiFd >= 0) {
        uint8_t silence[144];
        for (unsigned ch = 0; ch < 16; ++ch) {
            const uint8_t controls[] = {64, 123, 120};
            for (unsigned i = 0; i < 3; ++i) {
                silence[ch * 9 + i * 3] = 0xb0 | ch;
                silence[ch * 9 + i * 3 + 1] = controls[i];
                silence[ch * 9 + i * 3 + 2] = 0;
            }
        }
        ssize_t n = write(midiFd, silence, sizeof(silence));
        if (n != (ssize_t)sizeof(silence)) {
            printf("[MIDI] [WARN] Could not silence USB MIDI: %s\n", n < 0 ? strerror(errno) : "short write");
        }
        close(midiFd);
    }
    midiFd = keyboardFd = -1;
    pendingSize = chordCount = 0;
    keyDown = keyHeld = false;
    memset(notePolicy, 0, sizeof(notePolicy));
}

void MidiUsbOutput::forward(const uint8_t *bytes, size_t count, bool mapped) {
    if (!mode || midiFd < 0 || !count) return;
    bool pass = mode == 2 || !mapped;
    unsigned kind = bytes[0] & 0xf0, channel = bytes[0] & 15;
    if (count == 3 && (kind == 0x80 || kind == 0x90)) {
        bool on = kind == 0x90 && bytes[2] != 0;
        auto &policy = notePolicy[channel][bytes[1]];
        if (!on && policy) pass = policy == 2;
        if (on) policy = pass ? 2 : 1;
        else policy = 0;
    }
    if (!pass) return;
    if (count > sizeof(pending) - pendingSize) {
        printf("[MIDI] [WARN] USB output queue full; resetting output to avoid stuck notes\n");
        closeDevices();
        return;
    }
    memcpy(pending + pendingSize, bytes, count);
    pendingSize += count;
}

void MidiUsbOutput::trigger(const char *keys) {
    if (!keyboardEnabled || keyboardFd < 0) return;
    uint8_t report[8];
    if (!midiChordReport(keys, report)) {
        printf("[MIDI] [WARN] Invalid keyboard chord: %s\n", keys);
        return;
    }
    (void)queueKeyboardReport(report);
}

bool MidiUsbOutput::queueKeyboardReport(const uint8_t (&report)[8], bool hold) {
    if (!keyboardEnabled || keyboardFd < 0) return false;
    if (chordCount == MIDI_CHORDS_MAX) {
        printf("[MIDI] [WARN] Keyboard chord queue full; dropping trigger\n");
        return false;
    }
    memcpy(chords[chordCount], report, sizeof(report));
    chordHolds[chordCount++] = hold;
    return true;
}

bool MidiUsbOutput::press(const char *keys) {
    uint8_t report[8];
    if (!midiChordReport(keys, report, true)) {
        printf("[MIDI] [WARN] Invalid held keyboard chord: %s\n", keys);
        return false;
    }
    return queueKeyboardReport(report, true);
}

bool MidiUsbOutput::release() {
    uint8_t report[8]{};
    return queueKeyboardReport(report);
}

void MidiUsbOutput::poll(uint64_t now, uint8_t passthrough, bool keyboard) {
    if (mode != passthrough || keyboardEnabled != keyboard) {
        closeDevices();
        mode = passthrough;
        keyboardEnabled = keyboard;
        nextProbeMs = 0;
    }
    if (!mode && !keyboardEnabled) return;
    if (now >= nextProbeMs) {
        nextProbeMs = now + AUDIOX_DEVICE_POLL_INTERVAL_MS;
        if (!usbHostConfigured()) {
            if (midiFd >= 0 || keyboardFd >= 0) closeDevices();
            return;
        }
        if (mode && midiFd < 0) {
            DIR *dir = opendir("/dev/snd");
            if (dir) {
                while (dirent *entry = readdir(dir)) {
                    if (!midiGadgetNode(entry->d_name)) continue;
                    char path[288];
                    snprintf(path, sizeof(path), "/dev/snd/%s", entry->d_name);
                    midiFd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                    if (midiFd < 0) printf("[MIDI] [WARN] Cannot open USB MIDI output: %s\n", strerror(errno));
                    break;
                }
                closedir(dir);
            }
        }
        if (keyboardEnabled && keyboardFd < 0) {
            keyboardFd = open("/dev/hidg0", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
            if (keyboardFd < 0 && errno != ENOENT) {
                printf("[MIDI] [WARN] Cannot open USB keyboard: %s\n", strerror(errno));
            } else if (keyboardFd >= 0) {
                keyDown = true;
                releaseMs = now;
            }
        }
    }
    if (midiFd >= 0 && pendingSize) {
        ssize_t n = write(midiFd, pending, pendingSize);
        if (n > 0) {
            pendingSize -= (size_t)n;
            memmove(pending, pending + n, pendingSize);
        } else if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
            printf("[MIDI] [WARN] USB MIDI write failed: %s\n", n < 0 ? strerror(errno) : "zero-length write");
            closeDevices();
        }
    }
    if (keyboardFd < 0 || (keyDown && !keyHeld && now < releaseMs)) return;
    bool automaticRelease = keyDown && !keyHeld;
    if (!automaticRelease && !chordCount) return;
    uint8_t release[8]{};
    const uint8_t *report = automaticRelease ? release : chords[0];
    ssize_t n = write(keyboardFd, report, sizeof(release));
    if (n == (ssize_t)sizeof(release)) {
        if (automaticRelease) {
            keyDown = false;
            keyHeld = false;
        } else {
            keyDown = report[0] || report[2] || report[3] || report[4] ||
                      report[5] || report[6] || report[7];
            keyHeld = keyDown && chordHolds[0];
            if (keyDown) keyboardPressMs = now;
            releaseMs = now + 30;
            --chordCount;
            memmove(chords, chords + 1, chordCount * sizeof(chords[0]));
            memmove(chordHolds, chordHolds + 1, chordCount * sizeof(chordHolds[0]));
        }
    } else if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        printf("[MIDI] [WARN] USB keyboard write failed: %s\n", n < 0 ? strerror(errno) : "short report");
        closeDevices();
    }
}
