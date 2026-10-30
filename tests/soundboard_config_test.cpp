#include "audio/context.hpp"
#include "config/context.hpp"
#include "midi/context.hpp"
#include "touch/context.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <string>
#include <time.h>
#include <unistd.h>

static bool testClockEnabled = false;
static uint64_t testNowMs = 0;
static unsigned midiPathChecks = 0;
static bool midiPathPresent = true;
static constexpr const char *testMidiPath = "/audiox-test-midi-device";
static int limitedWriteFd = -1;
static size_t limitedWriteSize = SIZE_MAX;

extern "C" ssize_t write(int fd, const void *buffer, size_t count) {
    if (fd == limitedWriteFd && count > limitedWriteSize) count = limitedWriteSize;
    return syscall(SYS_write, fd, buffer, count);
}

extern "C" int clock_gettime(clockid_t clock, struct timespec *ts) noexcept {
    if (testClockEnabled && clock == CLOCK_MONOTONIC) {
        ts->tv_sec = testNowMs / 1000;
        ts->tv_nsec = (testNowMs % 1000) * 1000000;
        return 0;
    }
    if (testClockEnabled && clock == CLOCK_REALTIME) {
        ts->tv_sec = 1700000000 + testNowMs / 1000;
        ts->tv_nsec = (testNowMs % 1000) * 1000000;
        return 0;
    }
    return syscall(SYS_clock_gettime, clock, ts);
}

extern "C" int access(const char *path, int mode) noexcept {
    if (testClockEnabled && strcmp(path, testMidiPath) == 0) {
        ++midiPathChecks;
        if (midiPathPresent) return 0;
        errno = ENOENT;
        return -1;
    }
    return syscall(SYS_faccessat, AT_FDCWD, path, mode);
}

#include "../src/http/api.cpp"
#include "../src/framebuffer/ui.cpp"

namespace {

void touch(const char *path) {
    FILE *fp = fopen(path, "w");
    assert(fp);
    const unsigned char wav[] = {
        'R', 'I', 'F', 'F', 40, 0, 0, 0, 'W', 'A', 'V', 'E',
        'f', 'm', 't', ' ', 16, 0, 0, 0, 1, 0, 2, 0,
        0x80, 0xbb, 0, 0, 0, 0xee, 2, 0, 4, 0, 16, 0,
        'd', 'a', 't', 'a', 4, 0, 0, 0, 0, 0, 0, 0,
    };
    assert(fwrite(wav, 1, sizeof(wav), fp) == sizeof(wav));
    assert(fclose(fp) == 0);
}

void expectLights(int fd, uint8_t removedVelocity, uint8_t keptVelocity, uint8_t aliasVelocity = 0) {
    uint8_t messages[128 * 3];
    size_t received = 0;
    while (received < sizeof(messages)) {
        ssize_t n = read(fd, messages + received, sizeof(messages) - received);
        assert(n > 0);
        received += (size_t)n;
    }
    assert(messages[36 * 3] == 0x92);
    assert(messages[36 * 3 + 2] == removedVelocity);
    assert(messages[37 * 3 + 2] == aliasVelocity);
    assert(messages[38 * 3 + 2] == keptVelocity);
    assert(messages[39 * 3 + 2] == 5);
}

void expectDark(int fd) {
    uint8_t messages[128 * 3];
    size_t received = 0;
    while (received < sizeof(messages)) {
        ssize_t n = read(fd, messages + received, sizeof(messages) - received);
        assert(n > 0);
        received += (size_t)n;
    }
    for (int note = 0; note < 128; ++note) {
        assert(messages[note * 3] == 0x92);
        assert(messages[note * 3 + 1] == note);
        assert(messages[note * 3 + 2] == 0);
    }
}

std::string readMidiConfig() {
    std::ifstream file("midi_map.txt");
    assert(file);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

} // namespace

int main() {
    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    assert(cwd >= 0);
    char temp[] = "/tmp/audiox-soundboard-test-XXXXXX";
    assert(mkdtemp(temp));
    assert(chdir(temp) == 0);
    assert(mkdir("sfx", 0755) == 0);
    touch("sfx/keep.wav");
    touch("sfx/remove.wav");

    Audiox app;
    TouchContext touchscreen(nullptr);
    touchscreen.app = &app;
    errno = EDOM;
    touchscreen.poll();
    assert(errno == EDOM);
    assert(!touchscreen.state.is_pressed);

    int input[2];
    assert(pipe(input) == 0);
    assert(fcntl(input[0], F_SETFL, O_NONBLOCK) == 0);
    touchscreen.fd = input[0];
    const input_event events[] = {
        {{}, EV_ABS, ABS_X, 123},
        {{}, EV_ABS, ABS_Y, 456},
        {{}, EV_KEY, BTN_TOUCH, 1},
    };
    assert(write(input[1], events, sizeof(events)) == (ssize_t)sizeof(events));
    touchscreen.poll();
    assert(touchscreen.state.is_pressed);
    assert(touchscreen.state.x == 123 && touchscreen.state.y == 456);
    const input_event release = {{}, EV_KEY, BTN_TOUCH, 0};
    assert(write(input[1], &release, sizeof(release)) == (ssize_t)sizeof(release));
    touchscreen.poll();
    assert(!touchscreen.state.is_pressed);
    close(input[0]);
    close(input[1]);
    touchscreen.fd = -1;

    ConfigStore config(&app);
    FILE *fixture = fopen("config.txt", "w");
    assert(fixture);
    assert(fputs("\t usb_sample_rate\t =\t 44100\n"
                 " \t soundboard_mode \t=\t hold\t\n"
                 "\t framebuffer_view=\t soundboard\n", fixture) >= 0);
    assert(fclose(fixture) == 0);
    ConfigData indented = config.readConfigFile();
    assert(indented.sampleRate == 44100);
    assert(indented.soundboardMode == SOUNDBOARD_MODE_HOLD);
    assert(indented.framebufferSoundboard == 1);
    assert(indented.playbackChannels == 2 && indented.captureChannels == 2);

    fixture = fopen("midi_map.txt", "w");
    assert(fixture);
    assert(fputs("\t note_map\t=\t 38,\t keep.wav\t\n"
                 " \t sound_mode =\t keep.wav,\t hold\t\n", fixture) >= 0);
    assert(fclose(fixture) == 0);
    MidiMapData indentedMidi = config.readMidiMapFile();
    assert(indentedMidi.mappingCount == 1 && indentedMidi.mappings[0].note == 38);
    assert(strcmp(indentedMidi.mappings[0].sfxPath, "keep.wav") == 0);
    assert(indentedMidi.soundModeCount == 1);
    assert(indentedMidi.soundModes[0].mode == SOUNDBOARD_MODE_HOLD);
    char ordered[SOUNDBOARD_ORDER_MAX][MIDI_SFX_PATH_MAX] = {};
    uint32_t orderedCount = 0;
    assert(config.listSoundboardFiles(ordered, SOUNDBOARD_ORDER_MAX, &orderedCount) == RET_OK);
    assert(orderedCount == 2 && strcmp(ordered[0], "keep.wav") == 0);

    MidiMapData data = {};
    data.globalLight = {2, 12, 24, 5};
    data.mappingCount = 4;
    data.mappings[0] = {36, "remove.wav"};
    data.mappings[1] = {37, "remove.wav"};
    data.mappings[2] = {38, "keep.wav"};
    data.mappings[3] = {41, "already-missing.wav"};
    snprintf(data.mappings[1].sfxPath, sizeof(data.mappings[1].sfxPath), "%s/sfx/remove.wav", temp);
    data.soundLightCount = 3;
    data.soundLights[0] = {"remove.wav", 1, 1, 11, 22};
    data.soundLights[1] = {"keep.wav", 1, 1, 15, 25};
    data.soundLights[2] = {"already-missing.wav", 1, 1, 13, 23};
    data.soundModeCount = 3;
    data.soundModes[0] = {"remove.wav", SOUNDBOARD_MODE_HOLD};
    data.soundModes[1] = {"keep.wav", SOUNDBOARD_MODE_PLAY};
    data.soundModes[2] = {"already-missing.wav", SOUNDBOARD_MODE_HOLD};
    data.soundOrderCount = 3;
    data.soundOrder[0] = {"keep.wav"};
    data.soundOrder[1] = {"remove.wav"};
    data.soundOrder[2] = {"already-missing.wav"};
    data.actionMappingCount = 1;
    data.actionMappings[0] = {39, MIDI_ACTION_STOP_ALL};
    data.effectStateCount = 2;
    data.effectStates[0] = {"fx_7", "cut", 1, 0, 0, 0, 0};
    data.effectStates[1] = {"fx_8", "gain", 1, 1, 0, 0, 0};
    data.effectParamCount = 1;
    data.effectParams[0] = {"fx_7", "channels", 6};
    data.effectToggleMappingCount = 1;
    data.effectToggleMappings[0] = {40, "fx_7"};
    assert(config.writeMidiMapFile(&data) == RET_OK);

    {
        AudioContext audio(&app);
        audiox::effects::SlotParams cut = {};
        assert(audio.getEffectParams("fx_7", &cut) == RET_OK);
        assert(cut.type == audiox::effects::EFFECT_CUT && cut.enabled == 1 && cut.values[0] == 6);
        MidiContext midi(&app);
        assert(midi.cachedMidiMap.mappingCount == 3);
        assert(midi.cachedMidiMap.soundLightCount == 2);
        assert(midi.cachedMidiMap.soundModeCount == 2);
        assert(midi.cachedMidiMap.soundOrderCount == 2);
        int leds[2];
        assert(pipe(leds) == 0);
        midi.fd = leds[1];
        midi.connected = 1;
        midi.refreshAllLighting();
        expectLights(leds[0], 11, 15, 12);

        HttpServer http(&app);
        auto dimRequest = [&](const char *method, const char *body, const char *expected) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleMidiLightDim(&http, sockets[0], method, body, strlen(body)) == 0);
            char reply[4096] = {};
            assert(read(sockets[1], reply, sizeof(reply) - 1) > 0);
            assert(strstr(reply, expected));
            close(sockets[0]);
            close(sockets[1]);
        };
        const std::string savedLighting = readMidiConfig();
        dimRequest("GET", "", "\"dimmed\":false");
        dimRequest("POST", "dimmed=1", "\"dimmed\":true");
        expectDark(leds[0]);
        dimRequest("GET", "", "\"dimmed\":true");
        dimRequest("POST", "", "400 Bad Request");
        dimRequest("POST", "dimmed=2", "400 Bad Request");
        dimRequest("POST", "dimmed=true", "400 Bad Request");
        dimRequest("DELETE", "", "405 Method Not Allowed");
        assert(midi.lightsDimmed);
        midi.refreshAllLighting();
        expectDark(leds[0]);
        midi.holdActive = 1;
        midi.heldNote = 36;
        midi.samplerModeActive = 1;
        midi.cachedMidiMap = config.readMidiMapFile();
        midi.refreshLightingFromState();
        assert(fcntl(leds[0], F_SETFL, O_NONBLOCK) == 0);
        uint8_t unexpected;
        assert(read(leds[0], &unexpected, 1) == -1 && errno == EAGAIN);
        assert(fcntl(leds[0], F_SETFL, 0) == 0);
        midi.holdActive = 0;
        midi.heldNote = 255;
        midi.samplerModeActive = 0;
        midi.disconnect();
        close(leds[0]);
        assert(pipe(leds) == 0);
        midi.fd = leds[1];
        midi.connected = 1;
        midi.refreshAllLighting();
        expectDark(leds[0]);
        assert(readMidiConfig() == savedLighting);
        {
            MidiContext restarted(nullptr);
            assert(!restarted.lightsDimmed);
            restarted.cachedMidiMap = config.readMidiMapFile();
            restarted.fd = leds[1];
            restarted.connected = 1;
            restarted.refreshAllLighting();
            expectLights(leds[0], 11, 15, 12);
        }
        assert(midi.lightsDimmed);
        dimRequest("POST", "dimmed=0", "\"dimmed\":false");
        expectLights(leds[0], 11, 15, 12);
        app.midi = nullptr;
        dimRequest("GET", "", "503 Service Unavailable");
        app.midi = &midi;

        midi.heldNote = 36;
        midi.holdActive = 1;
        midi.samplerSelectedValid = 1;
        strcpy(midi.samplerSelectedSfx, "remove.wav");
        auto orderRequest = [&](const char *method, const char *body, const char *expected) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleSoundboardOrder(&http, sockets[0], method, body, strlen(body)) == 0);
            char reply[4096] = {};
            assert(read(sockets[1], reply, sizeof(reply) - 1) > 0);
            assert(strstr(reply, expected));
            close(sockets[0]);
            close(sockets[1]);
        };
        orderRequest("GET", "", "keep.wav\nremove.wav\n");
        orderRequest("POST", "remove.wav\nkeep.wav\n", "200 OK");
        orderRequest("GET", "", "remove.wav\nkeep.wav\n");
        assert(strcmp(config.readMidiMapFile().soundOrder[0].sfxPath, "remove.wav") == 0);
        assert(strcmp(midi.cachedMidiMap.soundOrder[0].sfxPath, "remove.wav") == 0);
        refreshSoundboardFiles(&config, 1000);
        assert(soundboardUi.count == 2 && strcmp(soundboardUi.files[0], "remove.wav") == 0);
        orderRequest("POST", "keep.wav\nkeep.wav\n", "400 Bad Request");
        orderRequest("POST", "keep.wav\n", "409 Conflict");
        orderRequest("POST", "../keep.wav\nremove.wav\n", "409 Conflict");
        orderRequest("POST", "\n", "400 Bad Request");
        orderRequest("DELETE", "", "405 Method Not Allowed");
        orderRequest("GET", "", "remove.wav\nkeep.wav\n");
        touch("sfx/new.WAV");
        assert(mkdir("sfx/not-a-file.wav", 0755) == 0);
        orderRequest("GET", "", "remove.wav\nkeep.wav\nnew.WAV\n");
        soundboardUi.wasDown = true;
        refreshSoundboardFiles(&config, 4000);
        assert(soundboardUi.count == 2);
        soundboardUi.wasDown = false;
        refreshSoundboardFiles(&config, 7000);
        assert(soundboardUi.count == 3 && strcmp(soundboardUi.files[2], "new.WAV") == 0);
        assert(unlink("sfx/new.WAV") == 0);
        assert(rmdir("sfx/not-a-file.wav") == 0);
        int response[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, response) == 0);
        assert(handleSystemInfo(&http, response[0], "GET", "/api/system/info") == 0);
        char systemReply[2048] = {};
        assert(read(response[1], systemReply, sizeof(systemReply) - 1) > 0);
        assert(strstr(systemReply, "200 OK"));
        char expectedBoard[32];
        snprintf(expectedBoard, sizeof(expectedBoard), "\"board\":\"%s\"", audioxDetectBoard());
        assert(strstr(systemReply, expectedBoard));
        close(response[0]);
        close(response[1]);
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, response) == 0);
        assert(handleApiDeleteRootfs(&http, response[0], "/api/rootfs/sfx/remove.wav") == 0);
        char reply[512] = {};
        assert(read(response[1], reply, sizeof(reply) - 1) > 0);
        assert(strstr(reply, "200 OK"));
        struct stat deleted;
        assert(stat("sfx/remove.wav", &deleted) < 0 && errno == ENOENT);
        close(response[0]);
        close(response[1]);
        expectLights(leds[0], 0, 15);
        assert(midi.holdActive == 0 && midi.heldNote == 255);
        assert(audio.pendingSfxHoldStops.load() == 1);
        assert(midi.samplerSelectedValid == 0 && !midi.samplerSelectedSfx[0]);

        MidiMapData saved = config.readMidiMapFile();
        assert(saved.mappingCount == 1 && saved.mappings[0].note == 38);
        assert(saved.soundLightCount == 1 && strcmp(saved.soundLights[0].sfxPath, "keep.wav") == 0);
        assert(saved.soundModeCount == 1 && strcmp(saved.soundModes[0].sfxPath, "keep.wav") == 0);
        assert(saved.soundOrderCount == 1 && strcmp(saved.soundOrder[0].sfxPath, "keep.wav") == 0);
        refreshSoundboardFiles(&config, 10000);
        assert(soundboardUi.count == 1 && strcmp(soundboardUi.files[0], "keep.wav") == 0);
        assert(saved.actionMappingCount == 1 && saved.effectToggleMappingCount == 1);
        assert(saved.effectStateCount == 2 && saved.effectParamCount == 1);
        assert(saved.effectParams[0].value == 6);

        // Repeated cleanup is harmless and bypassed Cut survives another reload.
        saved.effectStates[0].enabled = 0;
        assert(config.writeMidiMapFile(&saved) == RET_OK);
        audio.loadEffectsFromConfig();
        assert(audio.getEffectParams("fx_7", &cut) == RET_OK);
        assert(cut.type == audiox::effects::EFFECT_CUT && cut.enabled == 0 && cut.values[0] == 6);
        assert(midi.reloadSoundboardMappings() == RET_OK);
        expectLights(leds[0], 0, 15);

        MidiMapData failed = saved;
        failed.mappingCount = 3;
        failed.mappings[1] = {41, "missing.wav"};
        failed.mappings[2] = {42, "loop.wav"};
        assert(symlink("loop.wav", "sfx/loop.wav") == 0);
        assert(config.purgeMissingSoundMappings(&failed) == RET_ERR);
        assert(failed.mappingCount == 3);
        assert(config.readMidiMapFile().mappingCount == 1);
        assert(unlink("sfx/loop.wav") == 0);

        MidiMapData stale = config.readMidiMapFile();
        stale.mappingCount = 2;
        stale.mappings[1] = {42, "deleted-outside-app.wav"};
        assert(config.writeMidiMapFile(&stale) == RET_OK);
        midi.samplerSelectedValid = 1;
        strcpy(midi.samplerSelectedSfx, "keep.wav");
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, response) == 0);
        assert(handleSoundboardReload(&http, response[0], "POST", "/api/soundboard/reload") == 0);
        memset(reply, 0, sizeof(reply));
        assert(read(response[1], reply, sizeof(reply) - 1) > 0);
        assert(strstr(reply, "200 OK"));
        assert(config.readMidiMapFile().mappingCount == 1);
        assert(midi.samplerSelectedValid == 1);
        expectLights(leds[0], 0, 15);
        close(response[0]);
        close(response[1]);

        close(leds[0]);
        midi.disconnect();

        auto chordRequest = [&](const char *method, const char *body, const char *expected) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleMidiChords(&http, sockets[0], method, body, strlen(body)) == 0);
            char text[4096]{};
            assert(read(sockets[1], text, sizeof(text) - 1) > 0);
            assert(strstr(text, expected));
            close(sockets[0]);
            close(sockets[1]);
        };
        chordRequest("GET", "", "\"chords\":[]");
        auto noMacros = config.readMidiMapFile();
        {
            std::ofstream legacy("midi_map.txt", std::ios::app);
            legacy << "key_chord=3,50,ALT+TAB\n";
            legacy.close();
            assert(legacy);
        }
        auto legacyMacro = config.readMidiMapFile();
        assert(legacyMacro.chordCount == 1 && legacyMacro.chords[0].channel == 3);
        assert(legacyMacro.chords[0].id > 0 && !strcmp(legacyMacro.chords[0].name, "MIDI 3:50"));
        assert(legacyMacro.chords[0].midiEnabled);
        assert(config.writeMidiMapFile(&legacyMacro) == RET_OK);
        assert(config.readMidiMapFile().chords[0].id == legacyMacro.chords[0].id);
        assert(readMidiConfig().find("automation=") != std::string::npos);
        assert(!legacyMacro.chords[0].idleVelocity && !legacyMacro.chords[0].pressedVelocity);
        assert(!legacyMacro.chords[0].script[0]);
        {
            std::ofstream malformed("midi_map.txt", std::ios::app);
            malformed << "key_macro=1,61,1,2,BAD\"KEY," << midiHexEncode("tap(\"A\")") << "\n";
            malformed.close();
            assert(malformed);
        }
        assert(config.readMidiMapFile().chordCount == 1);
        assert(config.writeMidiMapFile(&noMacros) == RET_OK);
        chordRequest("POST", "channel=0&note=62&keys=CTRL+SHIFT+F13", "200 OK");
        assert(config.readMidiMapFile().chordCount == 1);
        chordRequest("POST", "channel=16&note=62&keys=CTRL+A", "400 Bad Request");
        chordRequest("POST", "channel=0&note=62&keys=CTRL+", "400 Bad Request");
        chordRequest("POST", "channel=0&note=62&keys=CTRL+F14", "200 OK");
        assert(config.readMidiMapFile().chordCount == 1);
        const std::string scriptTail = "\ntap(\"A\")";
        const std::string maxScript = "--" +
            std::string(MIDI_SCRIPT_MAX - 1 - 2 - scriptTail.size(), 'x') + scriptTail;
        std::string longRequest = "channel=0&note=62&keys=&script_hex=" + midiHexEncode(maxScript.c_str());
        chordRequest("POST", longRequest.c_str(), "200 OK");
        assert(!strcmp(config.readMidiMapFile().chords[0].script, maxScript.c_str()));
        std::string tooLong = maxScript + "\n";
        longRequest = "channel=0&note=62&keys=&script_hex=" + midiHexEncode(tooLong.c_str());
        chordRequest("POST", longRequest.c_str(), "400 Bad Request");
        assert(!strcmp(config.readMidiMapFile().chords[0].script, maxScript.c_str()));
        chordRequest("POST", "channel=0&note=62&keys=CTRL+F14", "200 OK");
        chordRequest("GET", "", "\"keys\":\"CTRL+F14\"");
        chordRequest("PUT", "", "405 Method Not Allowed");
        ConfigData usbConfig = config.readConfigFile();
        assert(!usbConfig.midiPassthrough && !usbConfig.keyboardMacros);
        usbConfig.midiPassthrough = 1;
        usbConfig.keyboardMacros = 1;
        assert(config.writeConfigFile(&usbConfig) == RET_OK);
        assert(config.readConfigFile().midiPassthrough == 1 && config.readConfigFile().keyboardMacros == 1);
        assert(!config.readMidiMapFile().samplerKeyboardEnabled);
        assert(mkdir("config.txt.tmp", 0755) == 0);
        ConfigData failedUsbConfig = usbConfig;
        failedUsbConfig.midiPassthrough = 2;
        assert(config.writeConfigFile(&failedUsbConfig) == RET_ERR);
        assert(config.readConfigFile().midiPassthrough == 1);
        assert(rmdir("config.txt.tmp") == 0);

        int midiInput[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, midiInput) == 0);
        midi.fd = midiInput[0];
        midi.connected = 1;
        strcpy(midi.devPath, testMidiPath);
        midi.nextProbeMs = 1000;
        midi.nextMidiMapReloadMs = 5000;
        testNowMs = 1000;
        testClockEnabled = true;
        midi.poll();
        assert(midiPathChecks == 1 && midi.nextProbeMs == 1500);
        int usbMidi[2], usbKeyboard[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, usbMidi) == 0);
        assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, usbKeyboard) == 0);
        midi.usb.midiFd = usbMidi[0];
        midi.usb.keyboardFd = usbKeyboard[0];
        midi.usb.nextProbeMs = UINT64_MAX;
        const uint8_t incoming[] = {
            0x80,38,0, 0x90,61,100, 0x80,61,0, // Mapped release suppressed, unmapped note pair forwarded.
            0x90,62,100, 0x80,62,0, // Macro trigger is consumed in unmapped mode.
            0xf8, 0xc0,7, 0xf2,1,2, // Non-note messages are not lost.
        };
        assert(write(midiInput[1], incoming, sizeof(incoming)) == (ssize_t)sizeof(incoming));
        testNowMs = 1001;
        midi.poll();
        testNowMs = 1002;
        midi.poll();
        uint8_t forwarded[32]{}, keyReport[8]{};
        const uint8_t expectedMidi[] = {0x90,61,100,0x80,61,0,0xf8,0xc0,7,0xf2,1,2};
        ssize_t forwardedCount = read(usbMidi[1], forwarded, sizeof(forwarded));
        if (forwardedCount != (ssize_t)sizeof(expectedMidi)) {
            fprintf(stderr, "Expected %zu forwarded MIDI bytes, got %zd:", sizeof(expectedMidi), forwardedCount);
            for (ssize_t i = 0; i < forwardedCount; ++i) fprintf(stderr, " %02x", forwarded[i]);
            fputc('\n', stderr);
        }
        assert(forwardedCount == (ssize_t)sizeof(expectedMidi));
        assert(memcmp(forwarded, expectedMidi, sizeof(expectedMidi)) == 0);
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        assert(keyReport[0] == 1 && keyReport[2] == 105);
        testNowMs = 1032;
        midi.poll();
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        for (auto byte : keyReport) assert(byte == 0);

        chordRequest("POST", "channel=0&note=62&keys=CTRL+A&idle_velocity=128", "400 Bad Request");
        chordRequest("POST", "channel=0&note=62&keys=&script_hex=00", "400 Bad Request");
        std::string badScript = "channel=0&note=62&keys=&script_hex=" + midiHexEncode("if then");
        chordRequest("POST", badScript.c_str(), "400 Bad Request");
        assert(!strcmp(config.readMidiMapFile().chords[0].keys, "CTRL+F14"));

        const char *script = "light(9)\ntap(\"CTRL+C\")\nwait(40)\nlight(4)\ntap(\"CTRL+V\")";
        std::string macroBody = "channel=0&note=62&keys=&idle_velocity=2&pressed_velocity=7&script_hex=" +
            midiHexEncode(script);
        chordRequest("POST", macroBody.c_str(), "200 OK");
        auto savedMacro = config.readMidiMapFile();
        assert(savedMacro.chords[0].idleVelocity == 2 && savedMacro.chords[0].pressedVelocity == 7);
        assert(!strcmp(savedMacro.chords[0].script, script));
        chordRequest("GET", "", "\"idle_velocity\":2,\"pressed_velocity\":7,\"script_hex\":");

        auto drainLights = [&]() {
            uint8_t messages[1024];
            while (read(midiInput[1], messages, sizeof(messages)) > 0) {}
            assert(errno == EAGAIN);
        };
        drainLights();
        const uint8_t macroOn[] = {0x90, 62, 100};
        assert(write(midiInput[1], macroOn, sizeof(macroOn)) == (ssize_t)sizeof(macroOn));
        testNowMs = 1040;
        midi.poll();
        assert(midi.macroPressed[0] && midi.ledStateByNote[62] == 9);
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        assert(keyReport[0] == 1 && keyReport[2] == 6); // CTRL+C
        testNowMs = 1069;
        midi.poll();
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) < 0 && errno == EAGAIN);
        testNowMs = 1070;
        midi.poll();
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        for (auto byte : keyReport) assert(byte == 0);
        const uint8_t macroOff[] = {0x90, 62, 0};
        assert(write(midiInput[1], macroOff, sizeof(macroOff)) == (ssize_t)sizeof(macroOff));
        testNowMs = 1071;
        midi.poll();
        assert(!midi.macroPressed[0] && midi.ledStateByNote[62] == 9);
        testNowMs = 1109;
        midi.poll();
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) < 0 && errno == EAGAIN);
        testNowMs = 1110;
        midi.poll();
        assert(midi.ledStateByNote[62] == 4);
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        assert(keyReport[0] == 1 && keyReport[2] == 25); // CTRL+V
        midi.setLightsDimmed(true);
        assert(midi.ledStateByNote[62] == 0);
        midi.setLightsDimmed(false);
        assert(midi.ledStateByNote[62] == 4);
        testNowMs = 1140;
        midi.poll();
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        for (auto byte : keyReport) assert(byte == 0);
        assert(midi.activeMacro == -1 && midi.ledStateByNote[62] == 2);
        for (unsigned i = 0; i < MIDI_CHORDS_MAX + 1; ++i) midi.triggerMacro(0);
        assert(midi.macroQueueCount == MIDI_CHORDS_MAX);
        midi.pollMacros(testNowMs);
        midi.macroDeadlineMs = testNowMs;
        midi.pollMacros(testNowMs);
        assert(midi.activeMacro == -1 && !midi.macroQueueCount && !midi.usb.chordCount);
        midi.triggerMacro(0);
        midi.connected = 0;
        midi.pollMacros(testNowMs + 10);
        assert(!midi.macroQueueCount && midi.activeMacro == -1);
        midi.connected = 1;

        // LED press/release and MIDI learn retain the source channel even when HID is idle.
        const uint8_t otherChannel[] = {0x99, 45, 80};
        assert(write(midiInput[1], otherChannel, sizeof(otherChannel)) == (ssize_t)sizeof(otherChannel));
        testNowMs = 1141;
        midi.poll();
        assert(midi.lastChannel == 9 && midi.lastNote == 45);
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, response) == 0);
        assert(handleMidiLastNote(&http, response[0], "GET", "/api/midi/last_note") == 0);
        memset(reply, 0, sizeof(reply));
        assert(read(response[1], reply, sizeof(reply) - 1) > 0);
        assert(strstr(reply, "\"last_channel\":9"));
        close(response[0]);
        close(response[1]);
        assert(read(usbMidi[1], forwarded, sizeof(forwarded)) == 3);

        // Editing/removing a running macro cancels queued steps and releases the active tap.
        assert(write(midiInput[1], macroOn, sizeof(macroOn)) == (ssize_t)sizeof(macroOn));
        testNowMs = 1162;
        midi.poll();
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        chordRequest("DELETE", "channel=0&note=62", "200 OK");
        testNowMs = 1163;
        midi.poll();
        assert(read(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        for (auto byte : keyReport) assert(byte == 0);
        assert(midi.activeMacro == -1 && midi.macroQueueCount == 0 && midi.ledStateByNote[62] == 0);
        assert(config.readMidiMapFile().chordCount == 0);
        chordRequest("DELETE", "channel=0&note=62", "200 OK");
        for (testNowMs = 1164; testNowMs < 1499; ++testNowMs) midi.poll();
        assert(midiPathChecks == 1 && midi.fd >= 0);

        // MIDI input remains responsive between device health checks.
        midiPathPresent = false;
        const uint8_t cc[] = {0xb0, 127, 42};
        assert(write(midiInput[1], cc, sizeof(cc)) == (ssize_t)sizeof(cc));
        midi.poll();
        assert(midi.lastCc == 127 && midi.lastCcValue == 42);
        assert(midiPathChecks == 1 && midi.fd >= 0);
        testNowMs = 1499;
        midi.poll();
        assert(read(usbMidi[1], forwarded, sizeof(forwarded)) == 3);
        testNowMs = 1500;
        midi.poll();
        assert(midiPathChecks == 2 && midi.fd < 0 && midi.nextProbeMs == 2000);
        for (testNowMs = 1501; testNowMs < 2000; ++testNowMs) midi.poll();
        assert(midiPathChecks == 2 && midi.nextProbeMs == 2000);
        close(midiInput[1]);
        close(usbMidi[1]);
        close(usbKeyboard[1]);

        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, midiInput) == 0);
        assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, usbKeyboard) == 0);
        midi.fd = midiInput[0];
        midi.connected = 1;
        midi.nextProbeMs = UINT64_MAX;
        midi.nextMidiMapReloadMs = UINT64_MAX;
        midi.usb.keyboardFd = usbKeyboard[0];
        midi.usb.nextProbeMs = UINT64_MAX;
        midi.macroUsbGeneration = midi.usb.sessionGeneration;

        auto saveScript = [&](const char *source) {
            std::string body = "channel=0&note=62&keys=&idle_velocity=2&pressed_velocity=7&script_hex=" +
                midiHexEncode(source);
            chordRequest("POST", body.c_str(), "200 OK");
            drainLights();
        };
        auto stepMacro = [&](uint64_t time) {
            testNowMs = time;
            midi.pollMacros(time);
            midi.usb.poll(time, 1, true);
        };
        auto expectReport = [&](uint8_t modifier, uint8_t usage) {
            uint8_t report[8]{};
            ssize_t count = read(usbKeyboard[1], report, sizeof(report));
            if (count != 8) fprintf(stderr, "Expected HID report modifier=%u usage=%u at %llu ms, got %zd; macro error: %s\n",
                                    modifier, usage, static_cast<unsigned long long>(testNowMs), count, midi.macroErrors[0]);
            assert(count == 8);
            assert(report[0] == modifier && report[2] == usage);
            for (unsigned i : {1U,3U,4U,5U,6U,7U}) assert(!report[i]);
        };
        auto noReport = [&]() {
            uint8_t report[8];
            assert(read(usbKeyboard[1], report, sizeof(report)) == -1 && errno == EAGAIN);
        };
        saveScript("state.count = (state.count or 0) + 1\n"
                   "light(event.velocity)\n"
                   "if event.channel ~= 0 or event.note ~= 62 or not event.pressed then unknown() end\n"
                   "if state.count == 1 then press('CTRL+A'); wait(100); release(); type('aA!\\n\\t',80)\n"
                   "else type('B',30) end");
        assert(write(midiInput[1], macroOn, sizeof(macroOn)) == (ssize_t)sizeof(macroOn));
        testNowMs = 2000;
        midi.poll();
        expectReport(1, 4);
        assert(midi.usb.keyHeld && midi.ledStateByNote[62] == 100);
        midi.triggerMacro(0, 50); // Capture the second event now; evaluate it after the first sequence.
        stepMacro(2001); // The hold wait starts after the press has reached HID.
        stepMacro(2100);
        noReport();
        assert(midi.usb.keyHeld);
        stepMacro(2101);
        expectReport(0, 0);
        stepMacro(2102);
        expectReport(0, 4);
        stepMacro(2132);
        expectReport(0, 0);
        stepMacro(2133);
        stepMacro(2181);
        noReport();
        stepMacro(2182);
        expectReport(2, 4);
        stepMacro(2212);
        expectReport(0, 0);
        stepMacro(2213);
        stepMacro(2262);
        expectReport(2, 30);
        stepMacro(2292);
        expectReport(0, 0);
        stepMacro(2293);
        stepMacro(2342);
        expectReport(0, 40);
        stepMacro(2372);
        expectReport(0, 0);
        stepMacro(2373);
        stepMacro(2422);
        expectReport(0, 43);
        stepMacro(2452);
        expectReport(0, 0);
        stepMacro(2453);
        assert(midi.activeMacro == -1 && midi.macroQueueCount == 1);
        stepMacro(2454);
        expectReport(2, 5);
        assert(midi.ledStateByNote[62] == 50);
        stepMacro(2484);
        expectReport(0, 0);
        stepMacro(2485);
        assert(midi.activeMacro == -1);

        // A press without an explicit release cannot leak into the next macro.
        saveScript("press('ALT')");
        midi.triggerMacro(0);
        stepMacro(2500);
        expectReport(4, 0);
        stepMacro(2501);
        expectReport(0, 0);
        stepMacro(2502);
        assert(midi.activeMacro == -1 && !midi.usb.keyDown);
        // tap/type replace, rather than accidentally inherit, a previous held chord.
        saveScript("press('CTRL'); tap('A')");
        midi.triggerMacro(0);
        stepMacro(2520);
        expectReport(1, 0);
        stepMacro(2521);
        expectReport(0, 0);
        stepMacro(2522);
        expectReport(0, 4);
        stepMacro(2552);
        expectReport(0, 0);
        stepMacro(2553);
        assert(midi.activeMacro == -1);

        // State is transactional: a failing event queues no partial actions or state mutation.
        saveScript("state.count=(state.count or 0)+1\n"
                   "if event.velocity == 100 then state.count=99; press('CTRL'); unknown() end\n"
                   "if state.count==1 then type('x') else type('y') end");
        midi.triggerMacro(0, 100);
        stepMacro(2570);
        noReport();
        assert(midi.activeMacro == -1 && midi.macroErrors[0][0]);
        const std::string errorField = "\"runtime_error_hex\":\"" + midiHexEncode(midi.macroErrors[0]);
        chordRequest("GET", "", errorField.c_str());
        midi.triggerMacro(0, 50);
        stepMacro(2580);
        expectReport(0, 27);
        assert(!midi.macroErrors[0][0]);
        stepMacro(2610);
        expectReport(0, 0);
        stepMacro(2611);
        // USB session loss resets persistent state even while no macro is running.
        ++midi.usb.sessionGeneration;
        stepMacro(2620);
        midi.triggerMacro(0, 50);
        stepMacro(2630);
        expectReport(0, 27);
        stepMacro(2660);
        expectReport(0, 0);
        stepMacro(2661);

        saveScript("while true do end");
        midi.triggerMacro(0);
        stepMacro(2680);
        assert(strstr(midi.macroErrors[0], "instruction limit"));
        noReport();
        assert(midi.activeMacro == -1 && !midi.usb.keyDown);
        saveScript("state.count=(state.count or 0)+1\n"
                   "log(string.format('You have pressed this %d times',state.count))\n"
                   "if state.last then log('It has been '..((millis()-state.last)//1000)..' seconds') end\n"
                   "state.last=millis()\n"
                   "log(tostring(epoch()))");
        testNowMs = 3000;
        midi.triggerMacro(0);
        stepMacro(3100);
        assert(!strcmp(midi.macroPlan.actions[0].text, "You have pressed this 1 times"));
        assert(!strcmp(midi.macroPlan.actions[1].text, "1700000003"));
        testNowMs = 6500;
        midi.triggerMacro(0);
        stepMacro(6900);
        assert(!strcmp(midi.macroPlan.actions[0].text, "You have pressed this 2 times"));
        assert(!strcmp(midi.macroPlan.actions[1].text, "It has been 3 seconds"));
        assert(!strcmp(midi.macroPlan.actions[2].text, "1700000006"));

        assert(audio.reloadSfxBank() == RET_OK);
        auto triggered = audio.sfxTriggerSeq.load();
        saveScript("sound.play('keep.wav',1.5)");
        midi.triggerMacro(0);
        stepMacro(7000);
        assert(audio.sfxTriggerSeq.load() == triggered + 1);
        assert(!strcmp(audio.sfxLastTriggeredBasename, "keep.wav"));
        assert(audio.sfxQueue[(audio.sfxQueueWrite.load() - 1) % AUDIO_SFX_QUEUE_CAP].pitchRatio == 1.5f);
        auto stopAllBefore = audio.pendingSfxStopAll.load();
        saveScript("sound.stop_all()");
        midi.triggerMacro(0);
        stepMacro(7020);
        assert(audio.pendingSfxStopAll.load() == stopAllBefore + 1);
        saveScript("sound.play('missing.wav')");
        midi.triggerMacro(0);
        stepMacro(7040);
        assert(strstr(midi.macroErrors[0], "sound.play failed") && midi.activeMacro == -1);

        drainLights();
        saveScript("midi.program(1,5); midi.note_on(1,60,90); wait(50); midi.note_off(1,60)");
        drainLights();
        midi.triggerMacro(0);
        stepMacro(7100);
        uint8_t controllerBytes[32]{};
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 5);
        const uint8_t startMidi[] = {0xc1,5,0x91,60,90};
        assert(!memcmp(controllerBytes, startMidi, sizeof(startMidi)));
        assert(midi.macroMidiNotes[1][60]);
        stepMacro(7149);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) < 0 && errno == EAGAIN);
        stepMacro(7150);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 3);
        assert(controllerBytes[0] == 0x81 && controllerBytes[1] == 60 && controllerBytes[2] == 0);
        assert(!midi.macroMidiNotes[1][60]);
        saveScript("midi.cc(2,64,127); midi.note_on(2,61,80)");
        drainLights();
        midi.triggerMacro(0);
        stepMacro(7200);
        const uint8_t cleanupMidi[] = {0xb2,64,127,0x92,61,80,0xb2,64,0,0x82,61,0};
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == (ssize_t)sizeof(cleanupMidi));
        assert(!memcmp(controllerBytes, cleanupMidi, sizeof(cleanupMidi)));
        saveScript("midi.note_on(2,61,80); wait(500)");
        drainLights();
        midi.triggerMacro(0);
        stepMacro(7250);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 3);
        midi.stopMacros();
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 3);
        assert(controllerBytes[0] == 0x82 && controllerBytes[1] == 61 && !controllerBytes[2]);

        saveScript("midi.send(248)");
        drainLights();
        // A real nonblocking socket under backpressure preserves the entire queued message.
        uint8_t fill[1024]{};
        while (write(midi.fd, fill, sizeof(fill)) > 0) {}
        assert(errno == EAGAIN);
        midi.triggerMacro(0);
        stepMacro(7300);
        assert(midi.controllerPendingSize == 1 && midi.controllerPending[0] == 248);
        drainLights();
        stepMacro(7301);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 1 && controllerBytes[0] == 248);
        assert(!midi.controllerPendingSize && midi.activeMacro == -1);

        saveScript("midi.note_on(0,60,90); wait(50); midi.note_off(0,60)");
        drainLights();
        limitedWriteFd = midi.fd;
        limitedWriteSize = 1;
        midi.triggerMacro(0);
        stepMacro(7350);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 1 && controllerBytes[0] == 0x90);
        assert(midi.controllerPendingSize == 2);
        stepMacro(7351);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 1 && controllerBytes[0] == 60);
        stepMacro(7352);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 1 && controllerBytes[0] == 90);
        limitedWriteFd = -1;
        limitedWriteSize = SIZE_MAX;
        stepMacro(7401);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) < 0 && errno == EAGAIN);
        stepMacro(7402);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 3 && controllerBytes[0] == 0x80);
        assert(midi.activeMacro == -1);

        // LED backpressure must not stall keyboard-only macros.
        saveScript("press('CTRL'); wait(5)");
        while (write(midi.fd, fill, sizeof(fill)) > 0) {}
        assert(errno == EAGAIN);
        midi.sendRawMidi(0x90, 62, 2);
        assert(midi.controllerPendingSize == 3);
        midi.triggerMacro(0);
        stepMacro(7415);
        expectReport(1, 0);
        stepMacro(7420);
        stepMacro(7425);
        expectReport(0, 0);
        stepMacro(7426);
        assert(midi.activeMacro == -1);
        drainLights();
        stepMacro(7427);
        drainLights();

        saveScript("press('GUI'); wait(500)");
        midi.triggerMacro(0);
        stepMacro(7450);
        expectReport(8, 0);
        midi.disconnect();
        expectReport(0, 0);
        assert(!midi.usb.keyHeld && midi.activeMacro == -1);

        // Named web automations do not need either USB Keyboard or a controller.
        midi.keyboardEnabled = false;
        auto macroRequest = [&](const char *method, const std::string &body, const char *expected, bool run = false) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleMidiChords(&http, sockets[0], method, body.data(), body.size(), true, run) == 0);
            std::string responseText;
            char responseBuffer[4096];
            ssize_t count;
            assert(shutdown(sockets[0], SHUT_WR) == 0);
            while ((count = read(sockets[1], responseBuffer, sizeof(responseBuffer))) > 0)
                responseText.append(responseBuffer, static_cast<size_t>(count));
            if (responseText.find(expected) == std::string::npos)
                fprintf(stderr, "Expected %s, got %s\n", expected, responseText.c_str());
            assert(responseText.find(expected) != std::string::npos);
            close(sockets[0]);
            close(sockets[1]);
        };
        auto saveNamed = [&](const char *name, const char *source, uint32_t id = 0, bool binding = false) {
            std::string body = "name_hex=" + midiHexEncode(name) +
                "&midi_enabled=" + (binding ? "1" : "0") + "&channel=0&note=62&keys=&script_hex=" +
                midiHexEncode(source);
            if (id) body += "&id=" + std::to_string(id);
            macroRequest("POST", body, "200 OK");
            auto saved = config.readMidiMapFile();
            return id ? id : saved.chords[saved.chordCount - 1].id;
        };
        uint32_t webId = saveNamed("Mix \"scene\" & one",
            "assert(event.source=='web'); state.count=(state.count or 0)+1; "
            "effects.enable('fx_7',false); effects.set('fx_7','channels',3); "
            "volume.set('sfx',75); volume.adjust('sfx',-5)");
        assert(webId > 0);
        macroRequest("GET", "", "\"name\":\"Mix \\\"scene\\\" & one\"");
        macroRequest("GET", "", "\"midi_enabled\":false");
        audio.routingGraph.thingCount = 1;
        strcpy(audio.routingGraph.things[0].id, "sfx");
        const auto persisted = readMidiConfig();
        const auto volumes = std::string("volumes.txt");
        assert(access(volumes.c_str(), F_OK) != 0 && errno == ENOENT);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8000);
        assert(midi.activeMacro == -1 && !midi.macroErrors[midi.compiledMacroCount - 1][0]);
        audiox::effects::SlotParams scriptedParams{};
        assert(audio.getEffectParams("fx_7", &scriptedParams) == RET_OK);
        assert(!scriptedParams.enabled && scriptedParams.values[0] == 3);
        assert(fabs(audio.getThingGain("sfx") - 0.7f) < 0.00001f);
        assert(readMidiConfig() == persisted);
        assert(access(volumes.c_str(), F_OK) != 0 && errno == ENOENT);
        // USB session churn must not reset or stall a keyboard-free script's state.
        ++midi.usb.sessionGeneration;
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8020);
        assert(midi.macroStates[midi.compiledMacroCount - 1].entries[0].integerValue == 2);
        uint32_t secondId = saveNamed("Web only", "effects.toggle('fx_7'); volume.adjust('sfx',100)");
        assert(secondId != webId);
        macroRequest("POST", "id=" + std::to_string(secondId), "200 OK", true);
        stepMacro(8040);
        assert(audio.getThingGain("sfx") == 1.0f);
        assert(audio.getEffectParams("fx_7", &scriptedParams) == RET_OK && scriptedParams.enabled);
        saveNamed("Renamed", "effects.set('fx_7','channels',99)", webId);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8060);
        assert(strstr(midi.macroErrors[1], "effect action failed"));
        assert(audio.getEffectParams("fx_7", &scriptedParams) == RET_OK && scriptedParams.values[0] == 3);
        saveNamed("Renamed", "volume.set('missing',50)", webId);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8080);
        assert(strstr(midi.macroErrors[1], "unknown audio thing ID"));
        saveNamed("Renamed", "state.count=1; tap('A')", webId);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8100);
        assert(strstr(midi.macroErrors[1], "keyboard actions require"));
        assert(!midi.macroStates[1].count);
        saveNamed("Renamed", "midi.send(248)", webId);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8120);
        assert(strstr(midi.macroErrors[1], "connected controller"));
        macroRequest("POST", "id=4294967296", "400 Bad Request", true);
        macroRequest("POST", "id=999", "404 Not Found", true);
        macroRequest("GET", "", "405 Method Not Allowed", true);
        macroRequest("DELETE", "id=" + std::to_string(webId), "200 OK");
        assert(config.readMidiMapFile().chords[1].id == secondId);
        macroRequest("DELETE", "id=" + std::to_string(secondId), "200 OK");
        std::string midiBody = "name_hex=" + midiHexEncode("MIDI effect toggle") +
            "&midi_enabled=1&channel=5&note=66&keys=&script_hex=" +
            midiHexEncode("assert(event.source=='midi' and event.channel==5 and event.velocity==80); "
                          "state.count=(state.count or 0)+1; effects.toggle('fx_7')");
        macroRequest("POST", midiBody, "200 OK");
        uint32_t midiId = config.readMidiMapFile().chords[1].id;
        // A bound note cannot be shared accidentally by two automations.
        macroRequest("POST", midiBody, "409 Conflict");
        usbConfig.keyboardMacros = 0;
        assert(config.writeConfigFile(&usbConfig) == RET_OK);
        midi.nextUsbConfigMs = 0;
        midi.nextMidiMapReloadMs = 9000;
        int automationInput[2];
        assert(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, automationInput) == 0);
        midi.fd = automationInput[0];
        midi.connected = 1;
        midi.nextProbeMs = 9000;
        const uint8_t automationNote[] = {0x95, 66, 80};
        assert(write(automationInput[1], automationNote, sizeof(automationNote)) == 3);
        testNowMs = 8200;
        midi.poll();
        assert(!midi.keyboardEnabled && midi.activeMacro == -1);
        assert(midi.macroStates[1].count == 1 && midi.macroStates[1].entries[0].integerValue == 1);
        assert(!midi.macroErrors[1][0]);
        for (unsigned i = 0; i < MIDI_CHORDS_MAX; ++i)
            macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        macroRequest("POST", "id=" + std::to_string(midiId), "409 Conflict", true);
        midi.disconnect();
        close(automationInput[1]);
        close(midiInput[1]);
        close(usbKeyboard[1]);
        testClockEnabled = false;
    }

    assert(unlink("sfx/keep.wav") == 0);
    assert(rmdir("sfx") == 0);
    assert(unlink("config.txt") == 0);
    assert(unlink("midi_map.txt") == 0);
    assert(unlink("fx_params.txt") == 0);
    assert(fchdir(cwd) == 0);
    close(cwd);
    assert(rmdir(temp) == 0);
    puts("soundboard ordering, framebuffer sync, cleanup, MIDI lighting, and Cut persistence tests passed");
}
