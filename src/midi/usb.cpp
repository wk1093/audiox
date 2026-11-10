#include "midi/usb.hpp"
#include "defs.hpp"

#include <cstdio>
#include <cstdlib>
#include <cerrno>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>

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

static ssize_t writeHidReport(int fd, bool media, const uint8_t *payload, size_t size) {
    uint8_t report[SCRIPT_HID_REPORT_LENGTH]{};
    report[0] = media ? SCRIPT_MEDIA_REPORT_ID : SCRIPT_KEYBOARD_REPORT_ID;
    memcpy(report + 1, payload, size);
    return write(fd, report, size + 1);
}

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
    if (midiFd >= 0 || keyboardFd >= 0 || mediaFd >= 0) ++sessionGeneration;
    if (mediaFd >= 0) {
        const uint8_t release[2]{};
        const ssize_t n = writeHidReport(mediaFd, true, release, sizeof(release));
        if (n != static_cast<ssize_t>(sizeof(release) + 1))
            printf("[SCRIPT] [WARN] Could not release USB media keys: %s\n",
                   n < 0 ? strerror(errno) : "short report");
        close(mediaFd);
    }
    if (keyboardFd >= 0) {
        uint8_t released[8]{};
        ssize_t n = writeHidReport(keyboardFd, false, released, sizeof(released));
        if (n != static_cast<ssize_t>(sizeof(released) + 1)) {
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
    midiFd = keyboardFd = mediaFd = -1;
    pendingSize = chordCount = 0;
    keyDown = keyHeld = currentMedia = false;
    mediaNeedsRelease = false;
    keyboardNeedsRelease = false;
    memset(notePolicy, 0, sizeof(notePolicy));
}

void MidiUsbOutput::forward(const uint8_t *bytes, size_t count, bool mapped, bool forcePass) {
    if (!mode || midiFd < 0 || !count) return;
    bool pass = forcePass || mode == 2 || !mapped;
    unsigned kind = bytes[0] & 0xf0, channel = bytes[0] & 15;
    if (count == 3 && (kind == 0x80 || kind == 0x90)) {
        bool on = kind == 0x90 && bytes[2] != 0;
        auto &policy = notePolicy[channel][bytes[1]];
        if (!on && policy && !forcePass) pass = policy == 2;
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

bool MidiUsbOutput::trigger(const char *keys) {
    if (const uint16_t usage = scriptMediaUsage(keys)) return queueMediaReport(usage);
    uint8_t report[8];
    if (!scriptChordReport(keys, report)) {
        printf("[MIDI] [WARN] Invalid keyboard chord: %s\n", keys);
        return false;
    }
    return queueKeyboardReport(report);
}

bool MidiUsbOutput::queueKeyboardReport(const uint8_t (&report)[8], bool hold) {
    if (!keyboardEnabled || keyboardFd < 0) return false;
    if (chordCount == SCRIPTS_MAX) {
        printf("[MIDI] [WARN] Keyboard chord queue full; dropping trigger\n");
        return false;
    }
    memcpy(chords[chordCount], report, sizeof(report));
    chordMedia[chordCount] = false;
    chordHolds[chordCount++] = hold;
    return true;
}

bool MidiUsbOutput::queueMediaReport(uint16_t usage, bool hold) {
    if (!keyboardEnabled || mediaFd < 0) {
        printf("[SCRIPT] [WARN] USB media keys require Consumer Control enabled and a connected host\n");
        return false;
    }
    if (chordCount == SCRIPTS_MAX) {
        printf("[SCRIPT] [WARN] Keyboard/media queue full; dropping trigger\n");
        return false;
    }
    memset(chords[chordCount], 0, sizeof(chords[0]));
    chords[chordCount][0] = static_cast<uint8_t>(usage);
    chords[chordCount][1] = static_cast<uint8_t>(usage >> 8);
    chordMedia[chordCount] = true;
    chordHolds[chordCount++] = hold;
    return true;
}

bool MidiUsbOutput::press(const char *keys) {
    if (const uint16_t usage = scriptMediaUsage(keys)) return queueMediaReport(usage, true);
    uint8_t report[8];
    if (!scriptChordReport(keys, report, true)) {
        printf("[MIDI] [WARN] Invalid held keyboard chord: %s\n", keys);
        return false;
    }
    return queueKeyboardReport(report, true);
}

bool MidiUsbOutput::release() {
    if (currentMedia) return queueMediaReport(0);
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
            if (midiFd >= 0 || keyboardFd >= 0 || mediaFd >= 0) closeDevices();
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
                keyboardNeedsRelease = true;
            }
        }
        if (keyboardEnabled && keyboardFd >= 0 && mediaFd < 0) {
            mediaFd = fcntl(keyboardFd, F_DUPFD_CLOEXEC, 0);
            if (mediaFd < 0)
                printf("[SCRIPT] [WARN] Cannot duplicate USB keyboard/media output: %s\n", strerror(errno));
            else if (mediaFd >= 0) mediaNeedsRelease = true;
        }
    }
    if (keyboardFd >= 0 && keyboardNeedsRelease) {
        const uint8_t release[8]{};
        const ssize_t n = writeHidReport(keyboardFd, false, release, sizeof(release));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        if (n != static_cast<ssize_t>(sizeof(release) + 1)) {
            printf("[SCRIPT] [WARN] Cannot initialize USB keyboard: %s\n",
                   n < 0 ? strerror(errno) : "short report");
            closeDevices();
            return;
        }
        keyboardNeedsRelease = false;
    }
    if (mediaFd >= 0 && mediaNeedsRelease) {
        const uint8_t release[2]{};
        const ssize_t n = writeHidReport(mediaFd, true, release, sizeof(release));
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        if (n != static_cast<ssize_t>(sizeof(release) + 1)) {
            printf("[SCRIPT] [WARN] Cannot initialize USB consumer control: %s\n",
                   n < 0 ? strerror(errno) : "short report");
            closeDevices();
            return;
        }
        mediaNeedsRelease = false;
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
    if (keyDown && !keyHeld && now < releaseMs) return;
    const bool switching = keyDown && chordCount && currentMedia != chordMedia[0];
    bool automaticRelease = keyDown && (!keyHeld || switching);
    if (!automaticRelease && !chordCount) return;
    uint8_t release[8]{};
    const uint8_t *report = automaticRelease ? release : chords[0];
    const bool media = automaticRelease ? currentMedia : chordMedia[0];
    const int outputFd = media ? mediaFd : keyboardFd;
    const size_t reportSize = media ? 2 : sizeof(release);
    if (outputFd < 0) {
        printf("[SCRIPT] [WARN] USB keyboard/media output unavailable\n");
        closeDevices();
        return;
    }
    ssize_t n = writeHidReport(outputFd, media, report, reportSize);
    if (n == static_cast<ssize_t>(reportSize + 1)) {
        if (automaticRelease) {
            keyDown = false;
            keyHeld = false;
        } else {
            currentMedia = media;
            keyDown = media ? (report[0] || report[1]) :
                (report[0] || report[2] || report[3] || report[4] ||
                 report[5] || report[6] || report[7]);
            keyHeld = keyDown && chordHolds[0];
            if (keyDown) keyboardPressMs = now;
            releaseMs = now + 30;
            --chordCount;
            memmove(chords, chords + 1, chordCount * sizeof(chords[0]));
            memmove(chordHolds, chordHolds + 1, chordCount * sizeof(chordHolds[0]));
            memmove(chordMedia, chordMedia + 1, chordCount * sizeof(chordMedia[0]));
        }
    } else if (n >= 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        printf("[SCRIPT] [WARN] USB keyboard/media write failed: %s\n", n < 0 ? strerror(errno) : "short report");
        closeDevices();
    }
}
