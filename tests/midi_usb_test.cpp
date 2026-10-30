#include "midi/usb.hpp"

#include <cassert>
#include <cstdio>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <climits>

static bool hostConnected = true;
static int limitedFd = -1;
static size_t writeLimit = SIZE_MAX;
static int writeError = 0;

extern "C" FILE *__real_fopen(const char *, const char *);
extern "C" FILE *__wrap_fopen(const char *path, const char *mode) {
    const char *text = nullptr;
    if (strstr(path, "/sys/class/udc/") == path) text = hostConnected ? "configured\n" : "not attached\n";
    if (strcmp(path, "/sys/class/sound/card7/id") == 0) text = "AudioXMIDI\n";
    if (strcmp(path, "/sys/class/sound/card8/id") == 0) text = "Controller\n";
    if (!text) return __real_fopen(path, mode);
    FILE *file = tmpfile();
    assert(file);
    assert(fputs(text, file) >= 0);
    rewind(file);
    return file;
}

extern "C" ssize_t __real_write(int, const void *, size_t);
extern "C" ssize_t __wrap_write(int fd, const void *bytes, size_t count) {
    if (fd == limitedFd) {
        if (writeError) { errno = writeError; return -1; }
        if (count > writeLimit) count = writeLimit;
    }
    return __real_write(fd, bytes, count);
}

static std::vector<uint8_t> receive(int fd, size_t count) {
    std::vector<uint8_t> result(count);
    assert(read(fd, result.data(), count) == (ssize_t)count);
    return result;
}

static void empty(int fd) {
    uint8_t byte;
    assert(read(fd, &byte, 1) == -1 && errno == EAGAIN);
}

int main() {
    uint8_t ascii[8];
    for (int c = 32; c <= 126; ++c) {
        assert(midiAsciiReport(static_cast<char>(c), ascii));
        assert((ascii[0] == 0 || ascii[0] == 2) && ascii[2] >= 4 && ascii[2] <= 56);
        for (unsigned i : {1U, 3U, 4U, 5U, 6U, 7U}) assert(!ascii[i]);
    }
    assert(midiAsciiReport('a', ascii) && ascii[0] == 0 && ascii[2] == 4);
    assert(midiAsciiReport('A', ascii) && ascii[0] == 2 && ascii[2] == 4);
    assert(midiAsciiReport('!', ascii) && ascii[0] == 2 && ascii[2] == 30);
    assert(midiAsciiReport('@', ascii) && ascii[0] == 2 && ascii[2] == 31);
    assert(midiAsciiReport('\\', ascii) && !ascii[0] && ascii[2] == 49);
    assert(midiAsciiReport('~', ascii) && ascii[0] == 2 && ascii[2] == 53);
    assert(midiAsciiReport('\n', ascii) && !ascii[0] && ascii[2] == 40);
    assert(midiAsciiReport('\t', ascii) && !ascii[0] && ascii[2] == 43);
    for (char c : {'\0', '\r', '\b', '\x7f', '\xff'}) assert(!midiAsciiReport(c, ascii));
    assert(midiChordReport("CTRL+SHIFT", ascii, true) && ascii[0] == 3 && !ascii[2]);
    assert(!midiChordReport("CTRL+SHIFT", ascii));
    assert(!midiChordReport("CTRL+", ascii, true));
    char decoded[32]{};
    const char *encodedSource = "tap(\"CTRL+A\")\n-- & = +";
    assert(midiHexDecode(midiHexEncode(encodedSource).c_str(), decoded, sizeof(decoded)));
    assert(!strcmp(decoded, encodedSource));
    for (const char *invalidHex : {"00", "1", "zz", "a0q2"}) {
        assert(!midiHexDecode(invalidHex, decoded, sizeof(decoded)));
    }
    assert(!midiHexDecode("6162", decoded, 2));
    assert(!midiHexDecode("", decoded, 0));
    assert(midiHexDecode("", decoded, sizeof(decoded)) && !decoded[0]);
    assert(midiGadgetNode("midiC7D0"));
    assert(!midiGadgetNode("midiC8D0"));
    assert(!midiGadgetNode("controlC7"));
    uint8_t report[8];
    assert(midiChordReport("CTRL+SHIFT+F13", report));
    assert(report[0] == 3 && report[2] == 104);
    assert(midiChordReport("gui+alt+0+F24+SPACE", report));
    assert(report[0] == 12 && report[2] == 39 && report[3] == 115 && report[4] == 44);
    for (const char *invalid : {"", "CTRL", "CTRL+", "+A", "A+A", "F0", "F25",
                              "A+B+C+D+E+F+G", "CTRL+ A", "ENTER\n", "a,b", "F 1", "F\t1"}) {
        assert(!midiChordReport(invalid, report));
    }

    MidiStream stream;
    std::vector<std::vector<uint8_t>> messages;
    auto emit = [&](const uint8_t *bytes, size_t count) {
        messages.emplace_back(bytes, bytes + count);
    };
    const uint8_t input[] = {
        0x90, 60, 100, 61, 0, 0xf8, 0xc2, 5, 6, 0xf2, 1, 2, 3,
        0xf0, 0x7d, 0xf8, 1, 0xf7, 0xf6, 0x80, 60, 0,
    };
    for (uint8_t byte : input) stream.feed(byte, emit);
    const std::vector<std::vector<uint8_t>> expected = {
        {0x90,60,100}, {0x90,61,0}, {0xf8}, {0xc2,5}, {0xc2,6}, {0xf2,1,2},
        {0xf8}, {0xf0,0x7d,1,0xf7}, {0xf6}, {0x80,60,0},
    };
    assert(messages == expected);
    messages.clear();
    stream.feed(0xf0, emit);
    for (int i = 0; i < 1022; ++i) stream.feed(1, emit);
    stream.feed(0xf7, emit);
    assert(messages.size() == 1 && messages[0].size() == 1024);
    messages.clear();
    stream.feed(0xf0, emit);
    for (int i = 0; i < 1023; ++i) stream.feed(1, emit);
    stream.feed(0xf7, emit);
    assert(messages.size() == 2 && messages[0].size() == 1024 &&
           messages[1] == std::vector<uint8_t>({0xf7}));
    messages.clear();
    for (uint8_t byte : {0x90, 60, 127}) stream.feed(byte, emit);
    assert(messages.size() == 1);

    int midi[2], keyboard[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, midi) == 0);
    assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, keyboard) == 0);
    {
        MidiUsbOutput usb;
        usb.mode = 1;
        usb.keyboardEnabled = true;
        usb.midiFd = midi[0];
        usb.keyboardFd = keyboard[0];
        usb.nextProbeMs = UINT64_MAX;
        const uint8_t mappedOn[] = {0x90,60,127}, mappedOff[] = {0x80,60,0};
        const uint8_t otherOn[] = {0x91,61,100}, otherOff[] = {0x91,61,0};
        usb.forward(mappedOn, 3, true);
        usb.forward(mappedOff, 3, false); // A changed mapping cannot leak a suppressed note-off.
        usb.forward(otherOn, 3, false);
        usb.forward(otherOff, 3, true); // Release a forwarded note even if it is now mapped.
        assert(usb.pendingSize == 6);
        usb.poll(0, 1, true);
        assert(receive(midi[1], 6) == std::vector<uint8_t>({0x91,61,100,0x91,61,0}));
        empty(midi[1]);
        usb.mode = 2;
        usb.forward(mappedOn, 3, true);
        limitedFd = midi[0];
        writeError = EAGAIN;
        usb.poll(1, 2, true);
        assert(usb.pendingSize == 3);
        empty(midi[1]);
        writeError = 0;
        writeLimit = 1;
        usb.poll(2, 2, true);
        assert(receive(midi[1], 1) == std::vector<uint8_t>({0x90}));
        assert(usb.pendingSize == 2);
        writeLimit = SIZE_MAX;
        usb.poll(3, 2, true);
        assert(receive(midi[1], 2) == std::vector<uint8_t>({60,127}));
        limitedFd = -1;

        usb.trigger("CTRL+SHIFT+F13");
        usb.trigger("ALT+TAB");
        usb.poll(10, 2, true);
        auto press = receive(keyboard[1], 8);
        assert(press[0] == 3 && press[2] == 104);
        usb.poll(39, 2, true);
        empty(keyboard[1]);
        usb.poll(40, 2, true);
        assert(receive(keyboard[1], 8) == std::vector<uint8_t>(8));
        usb.poll(50, 2, true);
        press = receive(keyboard[1], 8);
        assert(press[0] == 4 && press[2] == 43);
        hostConnected = false;
        usb.nextProbeMs = 51;
        usb.poll(51, 2, true);
        assert(receive(keyboard[1], 8) == std::vector<uint8_t>(8));
        auto silence = receive(midi[1], 144);
        assert(silence[1] == 64 && silence[4] == 123 && silence[7] == 120);
        assert(usb.midiFd == -1 && usb.keyboardFd == -1 && !usb.keyDown && !usb.chordCount);
        usb.poll(100, 0, false);
        usb.trigger("CTRL+A");
        assert(usb.chordCount == 0);
    }
    close(midi[1]);
    close(keyboard[1]);
    hostConnected = true;
    assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, midi) == 0);
    {
        MidiUsbOutput usb;
        usb.mode = 2;
        usb.midiFd = midi[0];
        usb.nextProbeMs = UINT64_MAX;
        uint8_t bytes[1024]{};
        bytes[0] = 0xf0;
        for (int i = 0; i < 4; ++i) usb.forward(bytes, sizeof(bytes), false);
        assert(usb.pendingSize == 4096);
        usb.forward(bytes, sizeof(bytes), false);
        assert(usb.midiFd == -1 && usb.pendingSize == 0);
        assert(receive(midi[1], 144).size() == 144);
    }
    close(midi[1]);
    assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, keyboard) == 0);
    {
        MidiUsbOutput usb;
        usb.keyboardEnabled = true;
        usb.keyboardFd = keyboard[0];
        usb.nextProbeMs = UINT64_MAX;
        for (unsigned i = 0; i < MIDI_CHORDS_MAX + 1; ++i) usb.trigger("CTRL+A");
        assert(usb.chordCount == MIDI_CHORDS_MAX);
        usb.poll(0, 0, true);
        assert(receive(keyboard[1], 8)[2] == 4);
        limitedFd = keyboard[0];
        writeError = EAGAIN;
        usb.poll(30, 0, true);
        assert(usb.keyDown);
        empty(keyboard[1]);
        writeError = 0;
        usb.poll(31, 0, true);
        assert(!usb.keyDown && receive(keyboard[1], 8) == std::vector<uint8_t>(8));
        limitedFd = -1;
    }
    close(keyboard[1]);
    assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, keyboard) == 0);
    {
        MidiUsbOutput usb;
        usb.keyboardEnabled = true;
        usb.keyboardFd = keyboard[0];
        usb.nextProbeMs = UINT64_MAX;
        assert(usb.press("CTRL"));
        limitedFd = keyboard[0];
        writeError = EAGAIN;
        usb.poll(0, 0, true);
        assert(usb.chordCount == 1 && !usb.keyDown);
        empty(keyboard[1]);
        writeError = 0;
        usb.poll(10, 0, true);
        assert(usb.keyDown && usb.keyHeld && usb.keyboardPressMs == 10);
        assert(receive(keyboard[1], 8) == std::vector<uint8_t>({1,0,0,0,0,0,0,0}));
        usb.poll(500, 0, true);
        empty(keyboard[1]);
        assert(usb.press("SHIFT+B"));
        usb.poll(510, 0, true);
        assert(receive(keyboard[1], 8) == std::vector<uint8_t>({2,0,5,0,0,0,0,0}));
        assert(usb.release());
        writeError = EAGAIN;
        usb.poll(520, 0, true);
        assert(usb.keyHeld && usb.chordCount == 1);
        empty(keyboard[1]);
        writeError = 0;
        usb.poll(530, 0, true);
        assert(receive(keyboard[1], 8) == std::vector<uint8_t>(8));
        assert(!usb.keyDown && !usb.keyHeld && !usb.chordCount);
        assert(usb.press("ALT+A"));
        usb.poll(540, 0, true);
        receive(keyboard[1], 8);
        usb.cancelChords();
        usb.poll(541, 0, true);
        assert(receive(keyboard[1], 8) == std::vector<uint8_t>(8));
        assert(!usb.keyDown && !usb.keyHeld);
        assert(usb.press("GUI"));
        usb.poll(550, 0, true);
        receive(keyboard[1], 8);
        usb.closeDevices();
        assert(receive(keyboard[1], 8) == std::vector<uint8_t>(8));
        assert(usb.sessionGeneration == 1 && !usb.keyHeld);
        limitedFd = -1;
    }
    close(keyboard[1]);
    puts("MIDI stream, filtering, note releases, and USB keyboard chord tests passed");
}
