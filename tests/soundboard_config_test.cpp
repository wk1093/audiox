#include "audio/context.hpp"
#include "config/context.hpp"
#include "midi/context.hpp"
#include "touch/context.hpp"
#include "script_device_stubs.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <linux/input.h>
#include <memory>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <string>
#include <time.h>
#include <unistd.h>
#include <thread>
#include <chrono>
#include <limits>

static bool testClockEnabled = false;
size_t BluetoothContext::copyRoutingDevices(BluetoothRoutingDevice *, size_t) const { return 0; }
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

static bool storageReadFails = false;
static int testStorageUsage(const char *path, struct statvfs *space) {
    if (storageReadFails) {
        errno = EIO;
        return -1;
    }
    return statvfs(path, space);
}

#define statvfs(...) testStorageUsage(__VA_ARGS__)
#include "../src/http/api.cpp"
#undef statvfs
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
    data.effectStates[0] = {"fx_7", "cut", 1, 0, 0, 0, 0, ""};
    data.effectStates[1] = {"fx_8", "gain", 1, 1, 0, 0, 0, ""};
    data.effectParamCount = 1;
    data.effectParams[0] = {"fx_7", "channels", 6};
    data.effectToggleMappingCount = 1;
    data.effectToggleMappings[0] = {40, "fx_7"};
    assert(config.writeMidiMapFile(&data) == RET_OK);

    {
        auto audioStorage = std::make_unique<AudioContext>(&app);
        auto &audio = *audioStorage;
        audiox::effects::SlotParams cut = {};
        assert(audio.getEffectParams("fx_7", &cut) == RET_OK);
        assert(cut.type == audiox::effects::EFFECT_CUT && cut.enabled == 1 && cut.values[0] == 6);
        auto midiStorage = std::make_unique<MidiContext>(&app);
        assert(app.scripts == &midiStorage->scripts);
        auto &midi = *midiStorage;
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
        uint8_t gridLedVelocity[128]{};
        auto layoutRequest = [&](const char *method, const char *body, const char *expected) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleSoundboardLayout(&http, sockets[0], method, body, strlen(body)) == 0);
            char reply[8192] = {};
            assert(read(sockets[1], reply, sizeof(reply) - 1) > 0);
            assert(strstr(reply, expected));
            close(sockets[0]);
            close(sockets[1]);
            assert(fcntl(leds[0], F_SETFL, O_NONBLOCK) == 0);
            uint8_t message[3];
            ssize_t bytes = 0;
            while ((bytes = read(leds[0], message, sizeof(message))) > 0) {
                assert(bytes == 3);
                gridLedVelocity[message[1]] = message[2];
            }
            assert(bytes == -1 && errno == EAGAIN);
            assert(fcntl(leds[0], F_SETFL, 0) == 0);
            return std::string(reply);
        };
        auto beforeLayoutStorage = std::make_unique<MidiMapData>(config.readMidiMapFile());
        auto& beforeLayout = *beforeLayoutStorage;
        layoutRequest("GET", "", "\"mode\":\"normal\"");
        layoutRequest("POST", "mode=grid&rows=2&columns=2", "\"mode\":\"grid\"");
        auto gridStorage = std::make_unique<MidiMapData>(config.readMidiMapFile());
        auto& grid = *gridStorage;
        assert(grid.soundboardLayout == SOUNDBOARD_LAYOUT_GRID);
        assert(grid.gridRows == 2 && grid.gridColumns == 2);
        assert(grid.mappingCount == beforeLayout.mappingCount);
        for (uint32_t i = 0; i < grid.mappingCount; ++i) {
            assert(grid.mappings[i].note == beforeLayout.mappings[i].note);
            assert(strcmp(grid.mappings[i].sfxPath, beforeLayout.mappings[i].sfxPath) == 0);
        }
        layoutRequest("POST", "corner=top_left&note=38", "200 OK");
        layoutRequest("POST", "corner=top_right&note=39", "200 OK");
        layoutRequest("POST", "corner=bottom_left&note=46", "200 OK");
        layoutRequest("POST", "corner=bottom_right&note=47", "200 OK");
        layoutRequest("GET", "", "\"sfx\":\"keep.wav\",\"row\":0,\"column\":0");
        grid = config.readMidiMapFile();
        uint8_t inferredNote = 0;
        assert(soundboardGridNote(&grid, 1, 1, &inferredNote) && inferredNote == 47);
        layoutRequest("POST", "sfx=keep.wav&row=1&column=0", "200 OK");
        grid = config.readMidiMapFile();
        assert(grid.gridCellCount == 2);
        assert(grid.mappingCount == 3);
        assert(grid.soundLightCount == beforeLayout.soundLightCount);
        assert(grid.soundModeCount == beforeLayout.soundModeCount);
        refreshSoundboardFiles(&config, 3500);
        assert(soundboardUi.gridMode && soundboardUi.gridRows == 2 && soundboardUi.gridColumns == 2);
        assert(soundboardFileAtGridCell(soundboardUi, 1, 0) == 1);
        bool keepMappedToGridPad = false;
        for (uint32_t i = 0; i < grid.mappingCount; ++i)
            if (strcmp(grid.mappings[i].sfxPath, "keep.wav") == 0 && grid.mappings[i].note == 46)
                keepMappedToGridPad = true;
        assert(keepMappedToGridPad);
        layoutRequest("POST", "sfx=remove.wav&row=1&column=0", "200 OK");
        grid = config.readMidiMapFile();
        assert(grid.mappingCount == 2);
        assert(midi.cachedMidiMap.mappingCount == grid.mappingCount);
        bool removeSwapped = false, keepSwapped = false;
        for (uint32_t i = 0; i < grid.mappingCount; ++i) {
            if (strcmp(grid.mappings[i].sfxPath, "remove.wav") == 0 && grid.mappings[i].note == 46)
                removeSwapped = true;
            if (strcmp(grid.mappings[i].sfxPath, "keep.wav") == 0 && grid.mappings[i].note == 39)
                keepSwapped = true;
        }
        assert(removeSwapped && keepSwapped);
        assert(gridLedVelocity[36] == 0 && gridLedVelocity[37] == 0);
        assert(gridLedVelocity[46] == 11);
        refreshSoundboardFiles(&config, 6000);
        assert(soundboardFileAtGridCell(soundboardUi, 1, 0) == 0);
        assert(soundboardFileAtGridCell(soundboardUi, 0, 1) == 1);
        layoutRequest("POST", "sfx=keep.wav&unassign=1", "200 OK");
        grid = config.readMidiMapFile();
        assert(grid.gridCellCount == 1 && grid.mappingCount == 1);
        assert(strcmp(grid.mappings[0].sfxPath, "remove.wav") == 0);
        assert(layoutRequest("GET", "", "200 OK").find("\"sfx\":\"keep.wav\"") == std::string::npos);
        layoutRequest("POST", "mode=normal", "200 OK");
        layoutRequest("POST", "mode=grid", "200 OK");
        assert(config.readMidiMapFile().gridCellCount == 1);
        touch("sfx/grid-new.wav");
        assert(layoutRequest("GET", "", "200 OK").find("\"sfx\":\"grid-new.wav\"") == std::string::npos);
        assert(unlink("sfx/grid-new.wav") == 0);
        layoutRequest("POST", "sfx=keep.wav&row=1&column=0", "200 OK");
        grid = config.readMidiMapFile();
        assert(grid.gridCellCount == 1 && grid.mappingCount == 1);
        assert(strcmp(grid.gridCells[0].sfxPath, "keep.wav") == 0);
        assert(strcmp(grid.mappings[0].sfxPath, "keep.wav") == 0 && grid.mappings[0].note == 46);
        layoutRequest("POST", "sfx=keep.wav&unassign=1", "200 OK");
        assert(config.readMidiMapFile().gridCellCount == 0);
        layoutRequest("POST", "mode=normal", "200 OK");
        layoutRequest("POST", "mode=grid", "200 OK");
        assert(config.readMidiMapFile().gridCellCount == 0);
        refreshSoundboardFiles(&config, 9000);
        assert(soundboardUi.gridMode && soundboardFileAtGridCell(soundboardUi, 1, 0) == -1);
        assert(soundboardFileAtGridCell(soundboardUi, 0, 0) == -1);
        layoutRequest("POST", "corner=top_right&note=40", "400 Bad Request");
        layoutRequest("POST", "", "400 Bad Request");
        layoutRequest("POST", "rows=2", "400 Bad Request");
        layoutRequest("POST", "sfx=keep.wav&unassign=0", "400 Bad Request");
        layoutRequest("POST", "sfx=keep.wav&unassign=1&row=0&column=0", "400 Bad Request");
        layoutRequest("POST", "sfx=keep.wav&row=1&column=0", "200 OK");
        layoutRequest("POST", "mode=grid&rows=1&columns=1", "200 OK");
        grid = config.readMidiMapFile();
        assert(grid.gridCornerMask == 0 && grid.gridCellCount == 0 && grid.mappingCount == 0);
        layoutRequest("POST", "mode=normal", "\"mode\":\"normal\"");
        assert(config.readMidiMapFile().soundboardLayout == SOUNDBOARD_LAYOUT_NORMAL);
        auto actionGridStorage = std::make_unique<MidiMapData>(beforeLayout);
        auto& actionGrid = *actionGridStorage;
        actionGrid.soundboardLayout = SOUNDBOARD_LAYOUT_GRID;
        actionGrid.gridInitialized = 1;
        actionGrid.gridRows = actionGrid.gridColumns = 2;
        actionGrid.gridCornerMask = 15;
        actionGrid.gridCornerNotes[0] = 38;
        actionGrid.gridCornerNotes[1] = 39;
        actionGrid.gridCornerNotes[2] = 46;
        actionGrid.gridCornerNotes[3] = 47;
        actionGrid.gridCellCount = 1;
        actionGrid.gridCells[0] = {"keep.wav", 0, 0};
        assert(config.writeMidiMapFile(&actionGrid) == RET_OK);
        layoutRequest("POST", "action=stop_all&row=0&column=1", "200 OK");
        layoutRequest("POST", "action=sampler_toggle&row=1&column=1", "200 OK");
        grid = config.readMidiMapFile();
        assert(grid.gridCellCount == 3 && grid.actionMappingCount == 2);
        assert(config.purgeMissingSoundMappings(&grid) == RET_OK && grid.gridCellCount == 3);
        layoutRequest("POST", "action=stop_all&row=0&column=0", "200 OK");
        grid = config.readMidiMapFile();
        bool stopAt38 = false, keepAt39 = false;
        for (uint32_t i = 0; i < grid.actionMappingCount; ++i)
            if (grid.actionMappings[i].action == MIDI_ACTION_STOP_ALL && grid.actionMappings[i].note == 38)
                stopAt38 = true;
        for (uint32_t i = 0; i < grid.mappingCount; ++i)
            if (strcmp(grid.mappings[i].sfxPath, "keep.wav") == 0 && grid.mappings[i].note == 39)
                keepAt39 = true;
        assert(stopAt38 && keepAt39 && gridLedVelocity[38] == 5);
        refreshSoundboardFiles(&config, 12000);
        const int stopIndex = soundboardFileAtGridCell(soundboardUi, 0, 0);
        const int samplerIndex = soundboardFileAtGridCell(soundboardUi, 1, 1);
        assert(stopIndex >= 0 && samplerIndex >= 0);
        assert(soundboardGridAction(soundboardUi.files[stopIndex]) == MIDI_ACTION_STOP_ALL);
        assert(soundboardGridAction(soundboardUi.files[samplerIndex]) == MIDI_ACTION_SAMPLER_TOGGLE);
        layoutRequest("POST", "action=sampler_toggle&row=0&column=0", "200 OK");
        grid = config.readMidiMapFile();
        bool samplerAt38 = false, stopAt47 = false;
        for (uint32_t i = 0; i < grid.actionMappingCount; ++i) {
            if (grid.actionMappings[i].action == MIDI_ACTION_SAMPLER_TOGGLE && grid.actionMappings[i].note == 38)
                samplerAt38 = true;
            if (grid.actionMappings[i].action == MIDI_ACTION_STOP_ALL && grid.actionMappings[i].note == 47)
                stopAt47 = true;
        }
        assert(samplerAt38 && stopAt47);
        layoutRequest("POST", "action=sampler_toggle&unassign=1", "200 OK");
        grid = config.readMidiMapFile();
        assert(grid.gridCellCount == 2 && grid.actionMappingCount == 1);
        assert(grid.actionMappings[0].action == MIDI_ACTION_STOP_ALL);
        layoutRequest("POST", "action=unknown&row=0&column=0", "400 Bad Request");
        layoutRequest("POST", "action=stop_all&sfx=keep.wav&row=0&column=0", "400 Bad Request");
        auto samplerRequest = [&](const char *method, const char *expected) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleMidiSamplerMode(&http, sockets[0], method) == 0);
            char reply[512]{};
            assert(read(sockets[1], reply, sizeof(reply) - 1) > 0);
            assert(strstr(reply, expected));
            close(sockets[0]);
            close(sockets[1]);
            // Drain any lighting changes produced by the runtime mode toggle.
            layoutRequest("GET", "", "200 OK");
        };
        samplerRequest("GET", "\"active\":false");
        samplerRequest("POST", "\"active\":true");
        assert(midi.samplerModeActive == 1);
        samplerRequest("POST", "\"active\":false");
        samplerRequest("PUT", "405 Method Not Allowed");
        app.midi = nullptr;
        samplerRequest("POST", "503 Service Unavailable");
        app.midi = &midi;
        auto freshGridStorage = std::make_unique<MidiMapData>(beforeLayout);
        auto& freshGrid = *freshGridStorage;
        freshGrid.gridInitialized = 0;
        freshGrid.gridCellCount = 0;
        assert(config.writeMidiMapFile(&freshGrid) == RET_OK);
        layoutRequest("POST", "mode=grid&rows=1&columns=1", "200 OK");
        grid = config.readMidiMapFile();
        assert(grid.gridCellCount == 1 && grid.gridRows == 1 && grid.gridColumns == 1);
        assert(grid.mappingCount == beforeLayout.mappingCount);
        char capacityFiles[SOUNDBOARD_GRID_ELEMENTS_MAX][MIDI_SFX_PATH_MAX]{};
        for (unsigned i = 0; i < SOUNDBOARD_GRID_CELLS_MAX; ++i)
            snprintf(capacityFiles[i], MIDI_SFX_PATH_MAX, "sound-%u.wav", i);
        strcpy(capacityFiles[SOUNDBOARD_GRID_CELLS_MAX], SOUNDBOARD_GRID_STOP_ALL_ID);
        strcpy(capacityFiles[SOUNDBOARD_GRID_CELLS_MAX + 1], SOUNDBOARD_GRID_SAMPLER_TOGGLE_ID);
        auto capacityGridStorage = std::make_unique<MidiMapData>();
        auto& capacityGrid = *capacityGridStorage;
        capacityGrid.gridRows = capacityGrid.gridColumns = 8;
        SoundboardGridCell capacityCells[SOUNDBOARD_GRID_CELLS_MAX]{};
        uint32_t capacityCount = 0;
        assert(buildSoundboardGridCells(capacityGrid, capacityFiles, SOUNDBOARD_GRID_ELEMENTS_MAX,
            capacityCells, &capacityCount, false, true) == RET_OK);
        assert(capacityCount == SOUNDBOARD_GRID_CELLS_MAX);
        for (uint32_t i = 0; i < capacityCount; ++i)
            assert(soundboardGridAction(capacityCells[i].sfxPath) == MIDI_ACTION_NONE);
        assert(config.writeMidiMapFile(&beforeLayout) == RET_OK);
        midi.cachedMidiMap = beforeLayout;
        soundboardUi.lastFilesMs = 0;
        refreshSoundboardFiles(&config, 1000);
        soundboardUi.lastFilesMs = 0;
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
        struct statvfs space{};
        assert(statvfs(ROOT_MOUNT_POINT, &space) == 0);
        char expectedStorage[96];
        snprintf(expectedStorage, sizeof(expectedStorage), "\"storage\":{\"total_bytes\":%llu,",
                 (unsigned long long)((uint64_t)space.f_blocks * space.f_frsize));
        assert(strstr(systemReply, expectedStorage));
        assert(strstr(systemReply, "\"used_bytes\":"));
        assert(strstr(systemReply, "\"free_bytes\":"));
        assert(strstr(systemReply, "\"reserved_bytes\":"));
        close(response[0]);
        close(response[1]);
        storageReadFails = true;
        assert(socketpair(AF_UNIX, SOCK_STREAM, 0, response) == 0);
        assert(handleSystemInfo(&http, response[0], "GET", "/api/system/info") == 0);
        memset(systemReply, 0, sizeof(systemReply));
        assert(read(response[1], systemReply, sizeof(systemReply) - 1) > 0);
        assert(strstr(systemReply, "\"storage\":null"));
        assert(strstr(systemReply, expectedBoard));
        close(response[0]);
        close(response[1]);
        storageReadFails = false;
        const bool beforeHistoryClock = testClockEnabled;
        const uint64_t beforeHistoryTime = testNowMs;
        testClockEnabled = true;
        testNowMs = 3000;
        auto historyRequest = [&](const char *method, const char *query) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleSystemHistory(&http, sockets[0], method, query) == RET_OK);
            char buffer[4096]{};
            assert(read(sockets[1], buffer, sizeof(buffer) - 1) > 0);
            close(sockets[0]);
            close(sockets[1]);
            return std::string(buffer);
        };
        assert(historyRequest("GET", "limit=1000").find("\"samples\":[]") != std::string::npos);
        audiox::system::CpuTimes historyCpu{100, 50};
        for (uint64_t i = 1; i <= 3; ++i) {
            historyCpu.total += 100;
            audiox::system::systemHistory.add(i * 1000, 55, &historyCpu, i, 1);
        }
        const auto recent = historyRequest("GET", "limit=2");
        assert(recent.find("200 OK") != std::string::npos);
        assert(recent.find("\"id\":1,") == std::string::npos);
        assert(recent.find("\"id\":2,") != std::string::npos);
        assert(recent.find("\"id\":3,") != std::string::npos);
        assert(recent.find("\"capacity\":600,\"retention_ms\":600000") != std::string::npos);
        assert(audiox::system::systemHistory.size() == 3);
        for (const char *invalid : {"limit=0", "limit=1001", "limit=-1", "limit=1.5", "limit=abc", "limit=2&limit=3", "foo=3"})
            assert(historyRequest("GET", invalid).find("400 Bad Request") != std::string::npos);
        assert(historyRequest("POST", "limit=2").find("405 Method Not Allowed") != std::string::npos);
        testNowMs = 602999;
        const auto expiring = historyRequest("GET", "limit=1000");
        assert(expiring.find("\"id\":2,") == std::string::npos);
        assert(expiring.find("\"id\":3,") != std::string::npos);
        testNowMs = 603000;
        assert(historyRequest("GET", "limit=600").find("\"samples\":[]") != std::string::npos);
        testNowMs = beforeHistoryTime;
        testClockEnabled = beforeHistoryClock;
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

        auto pluginRequest = [&](const char *method, const char *path, const std::string &body) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            std::thread request([&]() {
                int result;
                if (!strncmp(path, "/api/plugins", 12))
                    result = handlePlugins(&http, sockets[0], method, path, body.data(), body.size());
                else if (!strcmp(path, "/api/effect/create"))
                    result = handleEffectCreate(&http, sockets[0], method, path, body.data(), body.size());
                else if (!strcmp(path, "/api/effect/set"))
                    result = handleEffectSet(&http, sockets[0], method, path, body.data(), body.size());
                else if (!strcmp(path, "/api/effect/delete"))
                    result = handleEffectDelete(&http, sockets[0], method, path, body.data(), body.size());
                else result = handleEffects(&http, sockets[0], method, path);
                assert(result == 0);
                assert(shutdown(sockets[0], SHUT_WR) == 0);
            });
            std::string response;
            char chunk[4096];
            ssize_t got;
            while ((got = read(sockets[1], chunk, sizeof(chunk))) > 0) response.append(chunk, got);
            assert(got == 0);
            request.join();
            close(sockets[0]);
            close(sockets[1]);
            return response;
        };
        const auto savedBeforeBuiltinCount = config.readMidiMapFile().effectStateCount;
        for (const char *type : {"pitch", "compressor", "limiter", "eq", "multiband", "noise_reduction"}) {
            const auto created = pluginRequest("POST", "/api/effect/create", std::string("type=") + type);
            assert(created.find("200 OK") != std::string::npos);
            saved = config.readMidiMapFile();
            assert(saved.effectStateCount == savedBeforeBuiltinCount + 1);
            const auto &definition = saved.effectStates[saved.effectStateCount - 1];
            assert(!strcmp(definition.type, type));
            const std::string effectId = definition.effectId;
            const auto *spec = audiox::effects::effectTypeSpecFor(audiox::effects::effectTypeFromString(type));
            for (uint8_t index = 0; index < spec->paramCount; ++index) {
                const auto &param = spec->params[index];
                const float value = param.minValue + (param.maxValue - param.minValue) * .25f;
                assert(pluginRequest("POST", "/api/effect/set", "id=" + effectId +
                    "&param=" + param.name + "&value=" + std::to_string(value)).find("200 OK") != std::string::npos);
            }
            audio.loadEffectsFromConfig();
            audiox::effects::SlotParams restored{};
            assert(audio.getEffectParams(effectId.c_str(), &restored) == RET_OK);
            assert(restored.type == spec->type && restored.enabled);
            const auto metadata = pluginRequest("GET", "/api/effects", "");
            assert(metadata.find(std::string("\"type\":\"") + type + "\"") != std::string::npos);
            const char *label = !strcmp(type, "pitch") ? "Pitch Shift" : !strcmp(type, "compressor") ? "Compressor" :
                !strcmp(type, "limiter") ? "Limiter" : !strcmp(type, "eq") ? "Parametric EQ" :
                !strcmp(type, "multiband") ? "Multiband Compressor" : "Noise Reduction";
            assert(metadata.find(std::string("\"name\":\"") + label + " " + effectId.substr(3) +
                                  "\"") != std::string::npos);
            for (uint8_t index = 0; index < spec->paramCount; ++index) {
                const auto &param = spec->params[index];
                const float value = param.minValue + (param.maxValue - param.minValue) * .25f;
                assert(fabsf(restored.values[index] - (!strcmp(param.name, "quality") ? roundf(value) : value)) < 1e-4f);
                assert(metadata.find(std::string("\"name\":\"") + param.name + "\"") != std::string::npos);
                assert(audio.setEffectParamNormalized(effectId.c_str(), param.name, 1) == RET_OK);
                audiox::effects::SlotParams midiControlled{};
                assert(audio.getEffectParams(effectId.c_str(), &midiControlled) == RET_OK);
                assert(midiControlled.values[index] == param.maxValue);
            }
            assert(pluginRequest("POST", "/api/effect/delete", "id=" + effectId).find("200 OK") != std::string::npos);
            assert(config.readMidiMapFile().effectStateCount == savedBeforeBuiltinCount);
        }
        puts("New builtin HTTP creation, parameter metadata, full parameter persistence/reload and deletion tests passed");
        const auto emptyCatalog = pluginRequest("GET", "/api/plugins", "");
        if (emptyCatalog.find("\"plugins\":[]") == std::string::npos) fprintf(stderr, "%s\n", emptyCatalog.c_str());
        assert(emptyCatalog.find("\"plugins\":[]") != std::string::npos);
        assert(pluginRequest("PUT", "/api/plugins/install", "invalid").find("400 Bad Request") != std::string::npos);
        assert(pluginRequest("POST", "/api/plugins/remove", "id=../../config").find("400 Bad Request") != std::string::npos);
        assert(pluginRequest("GET", "/api/plugins/plugin_gain/assets/../midi_map.txt", "").find("404 Not Found") != std::string::npos);
        assert(pluginRequest("POST", "/api/effect/create", "type=plugin_missing").find("400 Bad Request") != std::string::npos);
        if (const char *fixture = getenv("AUDIOX_TEST_PLUGIN_PACKAGE")) {
            assert(chmod(".", 0755) == 0);
            std::ifstream input(fixture, std::ios::binary);
            assert(input);
            std::string packageBytes(std::istreambuf_iterator<char>(input), {});
            audiox::plugin::Package package;
            std::string error;
            assert(audiox::plugin::parsePackage(reinterpret_cast<const uint8_t *>(packageBytes.data()),
                packageBytes.size(), package, error));
            const std::string id = package.metadata.id;
            auto savedBefore = config.readMidiMapFile();
            char displayName[64]{};
            midiEffectDisplayName(savedBefore, "fx_99", "plugin_not_installed",
                                  displayName, sizeof(displayName));
            assert(!strcmp(displayName, "plugin_not_installed 99"));
            auto softclipMetadata = package.metadata;
            strcpy(softclipMetadata.id, "plugin_softclip");
            strcpy(softclipMetadata.name, "Example Soft Clip");
            assert(audiox::plugin::registerPackage(softclipMetadata, error));
            midiEffectDisplayName(savedBefore, "fx_99", "plugin_softclip",
                                  displayName, sizeof(displayName));
            assert(!strcmp(displayName, "Example Soft Clip 99"));
            auto typed = softclipMetadata;
            strcpy(typed.id, "plugin_typed");
            typed.paramCount = 3;
            for (unsigned p = 0; p < typed.paramCount; ++p) {
                auto& parameter = typed.params[p];
                snprintf(parameter.name, sizeof(parameter.name), "p%u", p);
                strcpy(parameter.label, "Typed");
                strcpy(parameter.type, p == 0 ? "integer" : p == 1 ? "boolean" : "enum");
                parameter.min = 0;
                parameter.max = p == 0 ? 10 : p == 1 ? 1 : 2;
                parameter.defaultValue = 0;
                parameter.step = 1;
            }
            strcpy(typed.params[2].options, "A~B~C");
            assert(audiox::plugin::registerPackage(typed, error));
            audiox::effects::SlotParams typedSlot{};
            typedSlot.type = audiox::plugin::findType(typed.id);
            for (unsigned p = 0; p < 3; ++p) {
                assert(audiox::effects::setSlotParamValue(&typedSlot, typed.params[p].name, 0.6f) == RET_OK);
                assert(typedSlot.values[p] == 1);
                assert(audiox::effects::setSlotParamNormalized(&typedSlot, typed.params[p].name, 0.6f) == RET_OK);
                assert(typedSlot.values[p] == roundf(typed.params[p].max * 0.6f));
            }
            typedSlot.values[2] = 1.6f;
            audiox::effects::clampSlotParams(&typedSlot);
            assert(typedSlot.values[2] == 2);
            auto namedPlugin = std::make_unique<MidiMapData>();
            namedPlugin->effectStateCount = 1;
            strcpy(namedPlugin->effectStates[0].effectId, "fx_99");
            strcpy(namedPlugin->effectStates[0].name, "Custom clip");
            midiEffectDisplayName(*namedPlugin, "fx_99", "plugin_softclip",
                                  displayName, sizeof(displayName));
            assert(!strcmp(displayName, "Custom clip"));
            auto withMissing = savedBefore;
            auto &missing = withMissing.effectStates[withMissing.effectStateCount++];
            missing = {};
            strcpy(missing.effectId, "fx_1234");
            strcpy(missing.type, id.c_str());
            missing.enabled = 1;
            auto &stored = withMissing.effectParams[withMissing.effectParamCount++];
            stored = {};
            strcpy(stored.effectId, missing.effectId);
            strcpy(stored.param, package.metadata.params[0].name);
            stored.value = 1.6f;
            audiox::effects::SlotParams unavailable{};
            unavailable.enabled = 1;
            unavailable.type = audiox::plugin::reserveType(id.c_str());
            upsertEffectStateFromRuntime(&withMissing, missing.effectId, unavailable);
            assert(withMissing.effectParamCount == savedBefore.effectParamCount + 1);
            assert(config.writeMidiMapFile(&withMissing) == RET_OK);
            audio.effectStates[missing.effectId] = unavailable;
            assert(pluginRequest("PUT", "/api/plugins/install", packageBytes).find("200 OK") != std::string::npos);
            assert(audiox::plugin::available(audiox::plugin::findType(id.c_str())));
            assert(pluginRequest("PUT", "/api/plugins/install", packageBytes).find("409 Conflict") != std::string::npos);
            assert(audiox::plugin::available(audiox::plugin::findType(id.c_str())));
            assert(pluginRequest("GET", "/api/plugins", "").find(id) != std::string::npos);
            const auto catalogResponse = pluginRequest("GET", "/api/plugins", "");
            assert(catalogResponse.find("\"description\":\"" +
                audiox::json::escape(package.metadata.params[0].description) + "\"") != std::string::npos);
            assert(catalogResponse.find("\"type\":\"float\"") != std::string::npos);
            assert(pluginRequest("POST", "/api/plugins/reload", "id=" + id).find("200 OK") != std::string::npos);
            audiox::effects::SlotParams restored{};
            assert(audio.getEffectParams("fx_1234", &restored) == RET_OK);
            assert(restored.pluginHost && restored.values[0] == 1.6f);
            assert(pluginRequest("POST", "/api/effect/delete", "id=fx_1234").find("200 OK") != std::string::npos);
            const std::string ui = pluginRequest("GET", ("/api/plugins/" + id + "/assets/index.html").c_str(), "");
            assert(ui.find("sandbox allow-scripts") != std::string::npos);
            assert(ui.find("connect-src 'none'") != std::string::npos);
            const std::string created = pluginRequest("POST", "/api/effect/create", "type=" + id);
            assert(created.find("200 OK") != std::string::npos);
            const auto persisted = config.readMidiMapFile();
            const auto &effect = persisted.effectStates[persisted.effectStateCount - 1];
            assert(!strcmp(effect.type, id.c_str()));
            const std::string effectId = effect.effectId;
            const auto effectsResponse = pluginRequest("GET", "/api/effects", "");
            assert(effectsResponse.find("\"plugin_status\":\"ready\"") != std::string::npos);
            const std::string expectedName = std::string(package.metadata.name) + " " + effectId.substr(3);
            assert(effectsResponse.find("\"name\":\"" + expectedName + "\"") != std::string::npos);
            assert(effectsResponse.find("\"description\":\"" +
                audiox::json::escape(package.metadata.params[0].description) + "\"") != std::string::npos);
            assert(pluginRequest("POST", "/api/plugins/remove", "id=" + id).find("409 Conflict") != std::string::npos);
            if (package.metadata.paramCount) {
                const auto &parameter = package.metadata.params[0];
                if (parameter.min <= 0.00002f && parameter.max >= 0.00002f) {
                    const auto tinyUpdate = pluginRequest("POST", "/api/effect/set",
                        "id=" + effectId + "&param=" + parameter.name + "&value=0.00002");
                    char expected[96]{};
                    snprintf(expected, sizeof(expected), "\"%s\":%.9g", parameter.name, double(0.00002f));
                    assert(tinyUpdate.find("200 OK") != std::string::npos);
                    assert(tinyUpdate.find(expected) != std::string::npos);
                    assert(pluginRequest("GET", "/api/effects", "").find(expected) != std::string::npos);
                }
                assert(pluginRequest("POST", "/api/effect/set",
                    "id=" + effectId + "&param=" + parameter.name + "&value=" +
                    std::to_string(parameter.max)).find("200 OK") != std::string::npos);
                audiox::effects::SlotParams params{};
                assert(audio.getEffectParams(effectId.c_str(), &params) == RET_OK);
                float value = 0;
                assert(audiox::effects::getSlotParamValue(params, parameter.name, &value) == RET_OK);
                assert(value == parameter.max);
            }
            assert(pluginRequest("POST", "/api/plugins/reload", "id=" + id).find("200 OK") != std::string::npos);
            assert(pluginRequest("POST", "/api/effect/delete", "id=" + effectId).find("200 OK") != std::string::npos);
            audio.pollPlugins();
            assert(pluginRequest("POST", "/api/plugins/remove", "id=" + id).find("200 OK") != std::string::npos);
            assert(!audiox::plugin::available(audiox::plugin::findType(id.c_str())));
            audiox::plugin::PackageMetadata wide{};
            strcpy(wide.id, "plugin_wide");
            strcpy(wide.name, "Wide metadata");
            strcpy(wide.version, "1");
            wide.paramCount = 32;
            for (unsigned i = 0; i < wide.paramCount; ++i) {
                auto &parameter = wide.params[i];
                snprintf(parameter.name, sizeof(parameter.name), "p%u", i);
                memset(parameter.label, '"', sizeof(parameter.label) - 1);
                parameter.min = -std::numeric_limits<float>::max();
                parameter.max = parameter.defaultValue = std::numeric_limits<float>::max();
            }
            assert(audiox::plugin::registerPackage(wide, error));
            auto wideSaved = std::make_unique<MidiMapData>(savedBefore);
            audiox::effects::SlotParams wideSlot{};
            wideSlot.type = audiox::plugin::findType(wide.id);
            audiox::effects::setSlotDefaultsForType(&wideSlot, wideSlot.type);
            assert(audiox::effects::setSlotParamValue(&wideSlot, "p31", 0.123f) == RET_OK);
            upsertEffectStateFromRuntime(wideSaved.get(), "fx_4000", wideSlot);
            assert(config.writeMidiMapFile(wideSaved.get()) == RET_OK);
            auto wideReloaded = std::make_unique<MidiMapData>(config.readMidiMapFile());
            bool foundLast = false;
            for (unsigned i = 0; i < wideReloaded->effectParamCount; ++i) {
                const auto& p = wideReloaded->effectParams[i];
                if (!strcmp(p.effectId, "fx_4000") && !strcmp(p.param, "p31")) {
                    assert(fabsf(p.value - 0.123f) < 0.000001f);
                    foundLast = true;
                }
            }
            assert(foundLast);
            assert(config.writeMidiMapFile(&savedBefore) == RET_OK);
            for (unsigned i = 0; i < 24; ++i) {
                auto &effect = audio.effectStates["fx_" + std::to_string(2000 + i)];
                effect.enabled = 1;
                effect.type = audiox::plugin::findType(wide.id);
                audiox::effects::setSlotDefaultsForType(&effect, effect.type);
            }
            const auto fullResponse = pluginRequest("GET", "/api/effects", "");
            assert(fullResponse.find("200 OK") != std::string::npos);
            assert(fullResponse.size() > 49152);
            assert(fullResponse.find("\"id\":\"fx_2023\"") != std::string::npos);
            assert(fullResponse.substr(fullResponse.size() - 3) == "]}\n");
            for (unsigned i = 0; i < 24; ++i)
                audio.effectStates.erase("fx_" + std::to_string(2000 + i));
            audiox::plugin::PackageMetadata precise{};
            strcpy(precise.id, "plugin_precise");
            strcpy(precise.name, "Precise metadata");
            strcpy(precise.version, "1");
            precise.paramCount = 1;
            auto &parameter = precise.params[0];
            strcpy(parameter.name, "constructor");
            strcpy(parameter.label, "Small amount");
            parameter.min = 0.000001f;
            parameter.max = 0.00003f;
            parameter.defaultValue = 0.00002f;
            assert(audiox::plugin::registerPackage(precise, error));
            auto &preciseEffect = audio.effectStates["fx_3000"];
            preciseEffect.type = audiox::plugin::findType(precise.id);
            audiox::effects::setSlotDefaultsForType(&preciseEffect, preciseEffect.type);
            const auto preciseResponse = pluginRequest("GET", "/api/effects", "");
            char expectedBounds[160]{};
            snprintf(expectedBounds, sizeof(expectedBounds), "\"min\":%.9g,\"max\":%.9g,\"default\":%.9g",
                     double(parameter.min), double(parameter.max), double(parameter.defaultValue));
            assert(preciseResponse.find(expectedBounds) != std::string::npos);
            audio.effectStates.erase("fx_3000");
            if (const char *hangModule = getenv("AUDIOX_TEST_PLUGIN_HANG_MODULE")) {
                std::ifstream library(hangModule, std::ios::binary);
                assert(library);
                const std::vector<uint8_t> module(std::istreambuf_iterator<char>(library), {});
                for (auto &entry : package.entries)
                    if (entry.kind == audiox::plugin::Module) entry.data = module;
                std::vector<uint8_t> hanging;
                assert(audiox::plugin::buildPackage(package.entries, hanging, error));
                assert(pluginRequest("PUT", "/api/plugins/install",
                    std::string(hanging.begin(), hanging.end())).find("200 OK") != std::string::npos);
                std::atomic<bool> finished{false};
                std::thread creation([&]() {
                    char createdId[64]{};
                    assert(audio.createEffect(id.c_str(), createdId, sizeof(createdId)) == RET_ERR);
                    finished.store(true);
                });
                unsigned reads = 0;
                while (!finished.load()) {
                    const auto start = std::chrono::steady_clock::now();
                    audiox::effects::SlotParams ignored{};
                    (void)audio.getEffectParams("missing", &ignored);
                    assert(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(50));
                    ++reads;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                }
                creation.join();
                assert(reads > 10);
                assert(pluginRequest("POST", "/api/plugins/remove", "id=" + id).find("200 OK") != std::string::npos);
            }
            assert(config.writeMidiMapFile(&savedBefore) == RET_OK);
            assert(unlink("plugins/.lock") == 0);
            assert(rmdir("plugins") == 0);
            assert(chmod(".", 0700) == 0);
        }

        auto chordRequest = [&](const char *method, const char *body, const char *expected) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleMidiChords(&http, sockets[0], method, body, strlen(body)) == 0);
            char text[4096]{};
            assert(read(sockets[1], text, sizeof(text) - 1) > 0);
            if (!strstr(text, expected)) fprintf(stderr, "Expected %s; response: %s\n", expected, text);
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
        assert(legacyMacro.chords[0].hookMode && !legacyMacro.chords[0].keys[0]);
        assert(strstr(legacyMacro.chords[0].script, "tap(\"ALT+TAB\")"));
        assert(legacyMacro.chords[0].eventMask == 129);
        {
            std::ofstream malformed("midi_map.txt", std::ios::app);
            malformed << "key_macro=1,61,1,2,BAD\"KEY," << scriptHexEncode("tap(\"A\")") << "\n";
            malformed.close();
            assert(malformed);
        }
        assert(config.readMidiMapFile().chordCount == 1);
        assert(config.writeMidiMapFile(&noMacros) == RET_OK);
        {
            std::ofstream legacy("midi_map.txt", std::ios::app);
            legacy << "key_macro=1,61,3,7,-," << scriptHexEncode("return -- trailing comment") << "\n";
            legacy << "automation=900,1,2,63,4,8," << scriptHexEncode("Imported") << ",-,"
                   << scriptHexEncode("state.value=event.type") << "\n";
            legacy << "automation_events=900,126\nautomation_config_write=900,1\n";
            legacy.close();
            const auto originalText = readMidiConfig();
            auto imported = std::make_unique<MidiMapData>(config.readMidiMapFile());
            assert(imported->chordCount == 2);
            assert(imported->chords[0].hookMode && imported->chords[0].eventMask == 129);
            assert(imported->chords[0].idleVelocity == 3 && imported->chords[0].pressedVelocity == 7);
            assert(!imported->chords[0].configWriteAllowed);
            assert(imported->chords[1].id == 900 && imported->chords[1].eventMask == 254);
            assert(imported->chords[1].configWriteAllowed && imported->chords[1].hookMode);
            assert(readMidiConfig() == originalText);
            assert(config.writeMidiMapFile(imported.get()) == RET_OK);
            auto reread = std::make_unique<MidiMapData>(config.readMidiMapFile());
            assert(scriptMacroEqual(imported->chords[0], reread->chords[0]));
            assert(scriptMacroEqual(imported->chords[1], reread->chords[1]));
        }
        assert(config.writeMidiMapFile(&noMacros) == RET_OK);
        chordRequest("POST", "channel=0&note=62&keys=CTRL+SHIFT+F13", "200 OK");
        assert(config.readMidiMapFile().chordCount == 1);
        chordRequest("POST", "channel=16&note=62&keys=CTRL+A", "400 Bad Request");
        chordRequest("POST", "channel=0&note=62&keys=CTRL+", "400 Bad Request");
        chordRequest("POST", "channel=0&note=62&keys=CTRL+F14", "200 OK");
        assert(config.readMidiMapFile().chordCount == 1);
        const std::string scriptTail = "\ntap(\"A\")";
        const std::string maxScript = "--" +
            std::string(SCRIPT_MAX - 1 - 2 - scriptTail.size(), 'x') + scriptTail;
        std::string longRequest = "channel=0&note=62&keys=&script_hex=" + scriptHexEncode(maxScript.c_str());
        chordRequest("POST", longRequest.c_str(), "200 OK");
        assert(strstr(config.readMidiMapFile().chords[0].script, maxScript.c_str()));
        std::string tooLong = maxScript + "\n";
        longRequest = "channel=0&note=62&keys=&script_hex=" + scriptHexEncode(tooLong.c_str());
        chordRequest("POST", longRequest.c_str(), "400 Bad Request");
        assert(strstr(config.readMidiMapFile().chords[0].script, maxScript.c_str()));
        chordRequest("POST", "channel=0&note=62&keys=CTRL+F14", "200 OK");
        chordRequest("GET", "", scriptHexEncode("tap(\"CTRL+F14\")").c_str());
        chordRequest("PUT", "", "405 Method Not Allowed");
        ConfigData usbConfig = config.readConfigFile();
        assert(!usbConfig.midiPassthrough && !usbConfig.keyboardMacros);
        usbConfig.midiPassthrough = 1;
        usbConfig.keyboardMacros = 1;
        assert(config.writeConfigFile(&usbConfig) == RET_OK);
        assert(config.readConfigFile().midiPassthrough == 1 && config.readConfigFile().keyboardMacros == 1);
        assert(config.readConfigFile().schemaVersion == CONFIG_SCHEMA_VERSION);
        {
            std::ifstream savedConfig("config.txt");
            assert(savedConfig);
            std::string firstLine;
            assert(std::getline(savedConfig, firstLine));
            assert(firstLine == "config_version=1");
        }
        assert(!config.readMidiMapFile().samplerKeyboardEnabled);
        assert(mkdir("config.txt.tmp", 0755) == 0);
        ConfigData failedUsbConfig = usbConfig;
        failedUsbConfig.midiPassthrough = 2;
        assert(config.writeConfigFile(&failedUsbConfig) == RET_ERR);
        assert(config.readConfigFile().midiPassthrough == 1);
        assert(rmdir("config.txt.tmp") == 0);

        auto updateNoticeRequest = [&](const char *method, const char *body, const char *expected) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleUpdateNotifications(&http, sockets[0], method, body, strlen(body)) == 0);
            char reply[1024] = {};
            assert(read(sockets[1], reply, sizeof(reply) - 1) > 0);
            assert(strstr(reply, expected));
            close(sockets[0]);
            close(sockets[1]);
        };
        assert(!config.readConfigFile().updateNotificationsOff);
        updateNoticeRequest("GET", "", "{\"enabled\":true}");
        updateNoticeRequest("POST", "enabled=0", "{\"enabled\":false}");
        assert(config.readConfigFile().updateNotificationsOff == 1);
        assert(config.readConfigFile().midiPassthrough == 1);
        updateNoticeRequest("GET", "", "{\"enabled\":false}");
        updateNoticeRequest("POST", "enabled=maybe", "400 Bad Request");
        updateNoticeRequest("DELETE", "", "405 Method Not Allowed");
        {
            std::ifstream savedConfig("config.txt");
            std::string text((std::istreambuf_iterator<char>(savedConfig)), std::istreambuf_iterator<char>());
            assert(text.find("\nupdate_notifications=0\n") != std::string::npos);
        }
        updateNoticeRequest("POST", "enabled=1", "{\"enabled\":true}");
        assert(!config.readConfigFile().updateNotificationsOff);

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
        midi.scripts.usb.midiFd = usbMidi[0];
        midi.scripts.usb.keyboardFd = usbKeyboard[0];
        midi.scripts.usb.nextProbeMs = UINT64_MAX;
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
        auto readKeyboardReport = [](int fd, uint8_t *payload, size_t size) -> ssize_t {
            assert(size == 8);
            uint8_t report[SCRIPT_HID_REPORT_LENGTH]{};
            const ssize_t n = read(fd, report, sizeof(report));
            if (n < 0) return n;
            assert(n == sizeof(report) && report[0] == SCRIPT_KEYBOARD_REPORT_ID);
            memcpy(payload, report + 1, size);
            return size;
        };
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
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        assert(keyReport[0] == 1 && keyReport[2] == 105);
        testNowMs = 1032;
        midi.poll();
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        for (auto byte : keyReport) assert(byte == 0);

        chordRequest("POST", "channel=0&note=62&keys=CTRL+A&idle_velocity=128", "400 Bad Request");
        chordRequest("POST", "channel=0&note=62&keys=&script_hex=00", "400 Bad Request");
        std::string badScript = "channel=0&note=62&keys=&script_hex=" + scriptHexEncode("if then");
        chordRequest("POST", badScript.c_str(), "400 Bad Request");
        assert(strstr(config.readMidiMapFile().chords[0].script, "tap(\"CTRL+F14\")"));

        const char *script = "light(9)\ntap(\"CTRL+C\")\nwait(40)\nlight(4)\ntap(\"CTRL+V\")";
        std::string macroBody = "channel=0&note=62&keys=&idle_velocity=2&pressed_velocity=7&script_hex=" +
            scriptHexEncode(script);
        chordRequest("POST", macroBody.c_str(), "200 OK");
        auto savedMacro = config.readMidiMapFile();
        assert(savedMacro.chords[0].idleVelocity == 2 && savedMacro.chords[0].pressedVelocity == 7);
        assert(strstr(savedMacro.chords[0].script, script));
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
        assert(midi.scripts.macroPressed[0] && midi.ledStateByNote[62] == 9);
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        assert(keyReport[0] == 1 && keyReport[2] == 6); // CTRL+C
        testNowMs = 1069;
        midi.poll();
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) < 0 && errno == EAGAIN);
        testNowMs = 1070;
        midi.poll();
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        for (auto byte : keyReport) assert(byte == 0);
        const uint8_t macroOff[] = {0x90, 62, 0};
        assert(write(midiInput[1], macroOff, sizeof(macroOff)) == (ssize_t)sizeof(macroOff));
        testNowMs = 1071;
        midi.poll();
        assert(!midi.scripts.macroPressed[0] && midi.ledStateByNote[62] == 9);
        testNowMs = 1109;
        midi.poll();
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) < 0 && errno == EAGAIN);
        testNowMs = 1110;
        midi.poll();
        assert(midi.ledStateByNote[62] == 4);
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        assert(keyReport[0] == 1 && keyReport[2] == 25); // CTRL+V
        midi.setLightsDimmed(true);
        assert(midi.ledStateByNote[62] == 0);
        midi.setLightsDimmed(false);
        assert(midi.ledStateByNote[62] == 4);
        testNowMs = 1140;
        midi.poll();
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        for (auto byte : keyReport) assert(byte == 0);
        assert(midi.scripts.activeMacro == -1 && midi.ledStateByNote[62] == 2);
        for (unsigned i = 0; i < SCRIPTS_MAX + 1; ++i) midi.scripts.triggerMacro(0);
        assert(midi.scripts.macroQueueCount == SCRIPTS_MAX);
        midi.scripts.pollMacros(testNowMs);
        midi.scripts.macroDeadlineMs = testNowMs;
        midi.scripts.pollMacros(testNowMs);
        assert(midi.scripts.activeMacro == -1 && !midi.scripts.macroQueueCount && !midi.scripts.usb.chordCount);
        midi.scripts.triggerMacro(0);
        midi.connected = 0;
        midi.scripts.pollMacros(testNowMs + 10);
        assert(!midi.scripts.macroQueueCount && midi.scripts.activeMacro == -1);
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
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        chordRequest("DELETE", "channel=0&note=62", "200 OK");
        testNowMs = 1163;
        midi.poll();
        assert(readKeyboardReport(usbKeyboard[1], keyReport, sizeof(keyReport)) == 8);
        for (auto byte : keyReport) assert(byte == 0);
        assert(midi.scripts.activeMacro == -1 && midi.scripts.macroQueueCount == 0 && midi.ledStateByNote[62] == 0);
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
        midi.scripts.usb.keyboardFd = usbKeyboard[0];
        midi.scripts.usb.nextProbeMs = UINT64_MAX;
        midi.scripts.macroUsbGeneration = midi.scripts.usb.sessionGeneration;

        auto saveScript = [&](const char *source) {
            std::string body = "channel=0&note=62&keys=&idle_velocity=2&pressed_velocity=7&script_hex=" +
                scriptHexEncode(source);
            chordRequest("POST", body.c_str(), "200 OK");
            drainLights();
        };
        auto stepMacro = [&](uint64_t time) {
            testNowMs = time;
            midi.scripts.pollMacros(time);
            midi.scripts.usb.poll(time, 1, true);
        };
        auto expectReport = [&](uint8_t modifier, uint8_t usage) {
            uint8_t report[8]{};
            ssize_t count = readKeyboardReport(usbKeyboard[1], report, sizeof(report));
            if (count != 8) fprintf(stderr, "Expected HID report modifier=%u usage=%u at %llu ms, got %zd; macro error: %s\n",
                                    modifier, usage, static_cast<unsigned long long>(testNowMs), count, midi.scripts.macroErrors[0]);
            assert(count == 8);
            assert(report[0] == modifier && report[2] == usage);
            for (unsigned i : {1U,3U,4U,5U,6U,7U}) assert(!report[i]);
        };
        auto noReport = [&]() {
            uint8_t report[8];
            assert(readKeyboardReport(usbKeyboard[1], report, sizeof(report)) == -1 && errno == EAGAIN);
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
        assert(midi.scripts.usb.keyHeld && midi.ledStateByNote[62] == 100);
        midi.scripts.triggerMacro(0, 50); // Capture the second event now; evaluate it after the first sequence.
        stepMacro(2001); // The hold wait starts after the press has reached HID.
        stepMacro(2100);
        noReport();
        assert(midi.scripts.usb.keyHeld);
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
        assert(midi.scripts.activeMacro == -1 && midi.scripts.macroQueueCount == 1);
        stepMacro(2454);
        expectReport(2, 5);
        assert(midi.ledStateByNote[62] == 50);
        stepMacro(2484);
        expectReport(0, 0);
        stepMacro(2485);
        assert(midi.scripts.activeMacro == -1);

        // A press without an explicit release cannot leak into the next macro.
        saveScript("press('ALT')");
        midi.scripts.triggerMacro(0);
        stepMacro(2500);
        expectReport(4, 0);
        stepMacro(2501);
        expectReport(0, 0);
        stepMacro(2502);
        assert(midi.scripts.activeMacro == -1 && !midi.scripts.usb.keyDown);
        // tap/type replace, rather than accidentally inherit, a previous held chord.
        saveScript("press('CTRL'); tap('A')");
        midi.scripts.triggerMacro(0);
        stepMacro(2520);
        expectReport(1, 0);
        stepMacro(2521);
        expectReport(0, 0);
        stepMacro(2522);
        expectReport(0, 4);
        stepMacro(2552);
        expectReport(0, 0);
        stepMacro(2553);
        assert(midi.scripts.activeMacro == -1);

        // State is transactional: a failing event queues no partial actions or state mutation.
        saveScript("state.count=(state.count or 0)+1\n"
                   "if event.velocity == 100 then state.count=99; press('CTRL'); unknown() end\n"
                   "if state.count==1 then type('x') else type('y') end");
        midi.scripts.triggerMacro(0, 100);
        stepMacro(2570);
        noReport();
        assert(midi.scripts.activeMacro == -1 && midi.scripts.macroErrors[0][0]);
        const std::string errorField = "\"runtime_error_hex\":\"" + scriptHexEncode(midi.scripts.macroErrors[0]);
        chordRequest("GET", "", errorField.c_str());
        midi.scripts.triggerMacro(0, 50);
        stepMacro(2580);
        expectReport(0, 27);
        assert(!midi.scripts.macroErrors[0][0]);
        stepMacro(2610);
        expectReport(0, 0);
        stepMacro(2611);
        // USB session loss resets persistent state even while no macro is running.
        ++midi.scripts.usb.sessionGeneration;
        stepMacro(2620);
        midi.scripts.triggerMacro(0, 50);
        stepMacro(2630);
        expectReport(0, 27);
        stepMacro(2660);
        expectReport(0, 0);
        stepMacro(2661);

        saveScript("while true do end");
        midi.scripts.triggerMacro(0);
        stepMacro(2680);
        assert(strstr(midi.scripts.macroErrors[0], "instruction limit"));
        noReport();
        assert(midi.scripts.activeMacro == -1 && !midi.scripts.usb.keyDown);
        saveScript("state.count=(state.count or 0)+1\n"
                   "log(string.format('You have pressed this %d times',state.count))\n"
                   "if state.last then log('It has been '..((millis()-state.last)//1000)..' seconds') end\n"
                   "state.last=millis()\n"
                   "log(tostring(epoch()))");
        testNowMs = 3000;
        midi.scripts.triggerMacro(0);
        stepMacro(3100);
        assert(!strcmp(midi.scripts.macroPlan.actions[0].text, "You have pressed this 1 times"));
        assert(!strcmp(midi.scripts.macroPlan.actions[1].text, "1700000003"));
        testNowMs = 6500;
        midi.scripts.triggerMacro(0);
        stepMacro(6900);
        assert(!strcmp(midi.scripts.macroPlan.actions[0].text, "You have pressed this 2 times"));
        assert(!strcmp(midi.scripts.macroPlan.actions[1].text, "It has been 3 seconds"));
        assert(!strcmp(midi.scripts.macroPlan.actions[2].text, "1700000006"));

        assert(audio.reloadSfxBank() == RET_OK);
        auto triggered = audio.sfxTriggerSeq.load();
        saveScript("sound.play('keep.wav',1.5)");
        midi.scripts.triggerMacro(0);
        stepMacro(7000);
        assert(audio.sfxTriggerSeq.load() == triggered + 1);
        assert(!strcmp(audio.sfxLastTriggeredBasename, "keep.wav"));
        assert(audio.sfxQueue[(audio.sfxQueueWrite.load() - 1) % AUDIO_SFX_QUEUE_CAP].pitchRatio == 1.5f);
        auto stopAllBefore = audio.pendingSfxStopAll.load();
        saveScript("sound.stop_all()");
        midi.scripts.triggerMacro(0);
        stepMacro(7020);
        assert(audio.pendingSfxStopAll.load() == stopAllBefore + 1);
        saveScript("sound.play('missing.wav')");
        midi.scripts.triggerMacro(0);
        stepMacro(7040);
        assert(strstr(midi.scripts.macroErrors[0], "sound.play failed") && midi.scripts.activeMacro == -1);

        drainLights();
        saveScript("midi.program(1,5); midi.note_on(1,60,90); wait(50); midi.note_off(1,60)");
        drainLights();
        midi.scripts.triggerMacro(0);
        stepMacro(7100);
        uint8_t controllerBytes[32]{};
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 5);
        const uint8_t startMidi[] = {0xc1,5,0x91,60,90};
        assert(!memcmp(controllerBytes, startMidi, sizeof(startMidi)));
        assert(midi.scripts.macroMidiNotes[1][60]);
        stepMacro(7149);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) < 0 && errno == EAGAIN);
        stepMacro(7150);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 3);
        assert(controllerBytes[0] == 0x81 && controllerBytes[1] == 60 && controllerBytes[2] == 0);
        assert(!midi.scripts.macroMidiNotes[1][60]);
        saveScript("midi.cc(2,64,127); midi.note_on(2,61,80)");
        drainLights();
        midi.scripts.triggerMacro(0);
        stepMacro(7200);
        const uint8_t cleanupMidi[] = {0xb2,64,127,0x92,61,80,0xb2,64,0,0x82,61,0};
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == (ssize_t)sizeof(cleanupMidi));
        assert(!memcmp(controllerBytes, cleanupMidi, sizeof(cleanupMidi)));
        saveScript("midi.note_on(2,61,80); wait(500)");
        drainLights();
        midi.scripts.triggerMacro(0);
        stepMacro(7250);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 3);
        midi.scripts.stopMacros();
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 3);
        assert(controllerBytes[0] == 0x82 && controllerBytes[1] == 61 && !controllerBytes[2]);

        saveScript("midi.send(248)");
        drainLights();
        // A real nonblocking socket under backpressure preserves the entire queued message.
        uint8_t fill[1024]{};
        while (write(midi.fd, fill, sizeof(fill)) > 0) {}
        assert(errno == EAGAIN);
        midi.scripts.triggerMacro(0);
        stepMacro(7300);
        assert(midi.scripts.controllerPendingSize == 1 && midi.scripts.controllerPending[0] == 248);
        drainLights();
        stepMacro(7301);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 1 && controllerBytes[0] == 248);
        assert(!midi.scripts.controllerPendingSize && midi.scripts.activeMacro == -1);

        saveScript("midi.note_on(0,60,90); wait(50); midi.note_off(0,60)");
        drainLights();
        limitedWriteFd = midi.fd;
        limitedWriteSize = 1;
        midi.scripts.triggerMacro(0);
        stepMacro(7350);
        assert(read(midiInput[1], controllerBytes, sizeof(controllerBytes)) == 1 && controllerBytes[0] == 0x90);
        assert(midi.scripts.controllerPendingSize == 2);
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
        assert(midi.scripts.activeMacro == -1);

        // LED backpressure must not stall keyboard-only macros.
        saveScript("press('CTRL'); wait(5)");
        while (write(midi.fd, fill, sizeof(fill)) > 0) {}
        assert(errno == EAGAIN);
        midi.sendRawMidi(0x90, 62, 2);
        assert(midi.scripts.controllerPendingSize == 3);
        midi.scripts.triggerMacro(0);
        stepMacro(7415);
        expectReport(1, 0);
        stepMacro(7420);
        stepMacro(7425);
        expectReport(0, 0);
        stepMacro(7426);
        assert(midi.scripts.activeMacro == -1);
        drainLights();
        stepMacro(7427);
        drainLights();

        saveScript("press('GUI'); wait(500)");
        midi.scripts.triggerMacro(0);
        stepMacro(7450);
        expectReport(8, 0);
        midi.disconnect();
        expectReport(0, 0);
        assert(!midi.scripts.usb.keyHeld && midi.scripts.activeMacro == -1);

        // Named web automations do not need either USB Keyboard or a controller.
        midi.keyboardEnabled = false;
        auto macroRequest = [&](const char *method, const std::string &body, const char *expected,
                                bool run = false, bool scripts = false) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleMidiChords(&http, sockets[0], method, body.data(), body.size(), true, run, scripts) == 0);
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
            std::string body = "name_hex=" + scriptHexEncode(name) +
                "&midi_enabled=" + (binding ? "1" : "0") + "&channel=0&note=62&keys=&script_hex=" +
                scriptHexEncode(source);
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
        assert(midi.scripts.activeMacro == -1 && !midi.scripts.macroErrors[midi.scripts.compiledMacroCount - 1][0]);
        audiox::effects::SlotParams scriptedParams{};
        assert(audio.getEffectParams("fx_7", &scriptedParams) == RET_OK);
        assert(!scriptedParams.enabled && scriptedParams.values[0] == 3);
        assert(fabs(audio.getThingGain("sfx") - 0.7f) < 0.00001f);
        assert(readMidiConfig() == persisted);
        assert(access(volumes.c_str(), F_OK) != 0 && errno == ENOENT);
        // USB session churn must not reset or stall a keyboard-free script's state.
        ++midi.scripts.usb.sessionGeneration;
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8020);
        assert(midi.scripts.macroStates[midi.scripts.compiledMacroCount - 1].entries[0].integerValue == 2);
        uint32_t secondId = saveNamed("Web only", "effects.toggle('fx_7'); volume.adjust('sfx',100)");
        assert(secondId != webId);
        macroRequest("POST", "id=" + std::to_string(secondId), "200 OK", true);
        stepMacro(8040);
        assert(audio.getThingGain("sfx") == 1.0f);
        assert(audio.getEffectParams("fx_7", &scriptedParams) == RET_OK && scriptedParams.enabled);
        saveNamed("Renamed", "effects.set('fx_7','channels',99)", webId);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8060);
        assert(strstr(midi.scripts.macroErrors[1], "effect action failed"));
        assert(audio.getEffectParams("fx_7", &scriptedParams) == RET_OK && scriptedParams.values[0] == 3);
        saveNamed("Renamed", "volume.set('missing',50)", webId);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8080);
        assert(strstr(midi.scripts.macroErrors[1], "unknown audio thing ID"));
        saveNamed("Renamed", "state.count=1; tap('A')", webId);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8100);
        assert(strstr(midi.scripts.macroErrors[1], "keyboard actions require"));
        assert(!midi.scripts.macroStates[1].count);
        saveNamed("Renamed", "midi.send(248)", webId);
        macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
        stepMacro(8120);
        assert(strstr(midi.scripts.macroErrors[1], "connected controller"));
        const auto beforeBuiltinScripts = scriptedParams;
        unsigned builtinScriptIndex = 0;
        for (const char *type : {"compressor", "limiter", "eq", "multiband", "noise_reduction"}) {
            const uint8_t effectType = audiox::effects::effectTypeFromString(type);
            const auto &param = audiox::effects::effectTypeSpecFor(effectType)->params[0];
            const float value = param.minValue + (param.maxValue - param.minValue) * .25f;
            assert(audio.setEffectType("fx_7", effectType) == RET_OK);
            const std::string source = std::string("effects.set('fx_7','") + param.name + "'," +
                std::to_string(value) + "); effects.enable('fx_7',false); effects.toggle('fx_7')";
            saveNamed("Renamed", source.c_str(), webId);
            const auto persistedSource = readMidiConfig();
            macroRequest("POST", "id=" + std::to_string(webId), "200 OK", true);
            stepMacro(8130 + 10 * builtinScriptIndex++);
            assert(!midi.scripts.macroErrors[1][0]);
            assert(audio.getEffectParams("fx_7", &scriptedParams) == RET_OK);
            assert(scriptedParams.type == effectType && scriptedParams.enabled);
            assert(fabsf(scriptedParams.values[0] - value) < 1e-4f);
            assert(readMidiConfig() == persistedSource);
        }
        assert(audio.setEffectType("fx_7", beforeBuiltinScripts.type) == RET_OK);
        assert(audio.setEffectParam("fx_7", "channels", beforeBuiltinScripts.values[0]) == RET_OK);
        macroRequest("POST", "id=4294967296", "400 Bad Request", true);
        macroRequest("POST", "id=999", "404 Not Found", true);
        macroRequest("GET", "", "405 Method Not Allowed", true);
        macroRequest("DELETE", "id=" + std::to_string(webId), "200 OK");
        assert(config.readMidiMapFile().chords[1].id == secondId);
        macroRequest("DELETE", "id=" + std::to_string(secondId), "200 OK");
        std::string midiBody = "name_hex=" + scriptHexEncode("MIDI effect toggle") +
            "&midi_enabled=1&channel=5&note=66&keys=&script_hex=" +
            scriptHexEncode("assert(event.source=='midi' and event.channel==5 and event.velocity==80); "
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
        assert(!midi.keyboardEnabled && midi.scripts.activeMacro == -1);
        assert(midi.scripts.macroStates[1].count == 1 && midi.scripts.macroStates[1].entries[0].integerValue == 1);
        assert(!midi.scripts.macroErrors[1][0]);
        assert(config.readMidiMapFile().chords[1].eventMask == 129);
        auto hookScript = [&](uint8_t mask, const char *source, bool binding = false) {
            std::string body = "id=" + std::to_string(midiId) + "&name_hex=" +
                scriptHexEncode("Event hook") + "&midi_enabled=" + (binding ? "1" : "0") +
                "&channel=5&note=66&keys=&event_mask=" + std::to_string(mask) +
                "&script_hex=" + scriptHexEncode(source);
            macroRequest("POST", body, "200 OK");
            assert(config.readMidiMapFile().chords[1].eventMask == (mask | 128));
        };
        hookScript(6, "state.count=(state.count or 0)+1; state.kind=event.type; "
                      "state.value=event.value; assert(not event.pressed)", true);
        macroRequest("POST", midiBody, "200 OK");
        uint32_t separatePressId = config.readMidiMapFile().chords[2].id;
        macroRequest("POST", midiBody, "409 Conflict");
        macroRequest("DELETE", "id=" + std::to_string(separatePressId), "200 OK");
        auto receiveMidi = [&](const uint8_t (&message)[3]) {
            assert(write(automationInput[1], message, sizeof(message)) == 3);
            testNowMs += 20;
            midi.poll();
        };
        auto hookState = [&](const char *key) -> const ScriptStateEntry & {
            for (unsigned i = 0; i < midi.scripts.macroStates[1].count; ++i)
                if (!strcmp(midi.scripts.macroStates[1].entries[i].key, key)) return midi.scripts.macroStates[1].entries[i];
            assert(false);
            return midi.scripts.macroStates[1].entries[0];
        };
        receiveMidi({0x95, 66, 100});
        assert(!midi.scripts.macroStates[1].count);
        receiveMidi({0x85, 66, 9});
        assert(hookState("count").integerValue == 1);
        receiveMidi({0x95, 66, 0});
        assert(hookState("count").integerValue == 2);
        receiveMidi({0xb5, 65, 42});
        assert(hookState("count").integerValue == 2);
        receiveMidi({0xb5, 66, 42});
        assert(hookState("count").integerValue == 3 && hookState("value").integerValue == 42);
        assert(!midi.scripts.macroErrors[1][0]);

        hookScript(24, "assert(event.source=='audio' and event.sound=='keep.wav'); "
                       "state.count=(state.count or 0)+1; state.pressed=event.pressed");
        audio.publishScriptSoundEvent("keep.wav", true);
        audio.publishScriptSoundEvent("keep.wav", false);
        for (unsigned i = 0; i < 3; ++i) {
            testNowMs += 20;
            midi.poll();
        }
        assert(hookState("count").integerValue == 2);
        assert(!hookState("pressed").booleanValue);

        hookScript(96, "assert(event.source=='device' and "
                       "event.pressed==(event.type=='device_connect')); state.kind=event.type; state.device=event.device");
        ScriptEvent connection{};
        connection.kind = ScriptEventKind::DeviceConnect;
        strcpy(connection.device, testMidiPath);
        midi.scripts.dispatchMacroEvent(connection);
        testNowMs += 20;
        midi.poll();
        assert(!strcmp(hookState("kind").stringValue, "device_connect"));
        midi.disconnect();
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(!strcmp(hookState("kind").stringValue, "device_disconnect"));
        assert(!midi.scripts.macroErrors[1][0]);
        close(automationInput[1]);

        auto saveCallbacks = [&](const std::string &source, const char *status = "200 OK",
                                 bool allowManagement = false) {
            std::string body = "id=" + std::to_string(midiId) +
                "&name_hex=" + scriptHexEncode("Multi-hook addon") +
                "&midi_enabled=1&channel=5&note=66&script_hex=" +
                scriptHexEncode(source.c_str());
            if (allowManagement) body += "&config_write_ack=1";
            macroRequest("POST", body, status, false, true);
        };
        std::string strictScript = "id=" + std::to_string(midiId) +
            "&name_hex=" + scriptHexEncode("Strict callbacks") +
            "&midi_enabled=0&channel=5&note=66&script_hex=" +
            scriptHexEncode("hook('run',function() end)");
        macroRequest("POST", strictScript + "&hook_mode=0", "400 Bad Request", false, true);
        macroRequest("POST", strictScript + "&keys=CTRL+A", "400 Bad Request", false, true);
        saveCallbacks("log('top-level sequence')", "400 Bad Request");
        saveCallbacks("", "400 Bad Request");
        saveCallbacks("hook('sound_finish',function(e) state.count=(state.count or 0)+1 end)");
        auto callbacksSaved = config.readMidiMapFile();
        assert(callbacksSaved.chords[1].hookMode && callbacksSaved.chords[1].eventMask == 16);
        macroRequest("GET", "", "\"hook_mode\":true");
        macroRequest("GET", "", "{\"scripts\":[", false, true);
        macroRequest("POST", "id=" + std::to_string(midiId), "callback script has no run hook", true);
        assert(!midi.scripts.macroQueueCount);
        saveCallbacks("hook('bogus',function() end)", "400 Bad Request");
        saveCallbacks("log('must not execute'); hook('run',function() end)", "400 Bad Request");
        assert(config.readMidiMapFile().chords[1].eventMask == 16);
        std::string addon =
            "local seen=0; "
            "local function count(e) seen=seen+1; assert(seen==1); "
            "state.count=(state.count or 0)+1; state.kind=e.type end; "
            "hook('run',count); hook('note_off',count); hook('cc',count); "
            "hook('sound_finish',count); hook('device_connect',count)";
        addon += "\n--" + std::string(SCRIPT_STORAGE_MAX - 1 - addon.size() - 3, 'x');
        assert(addon.size() == SCRIPT_STORAGE_MAX - 1);
        saveCallbacks(addon);
        callbacksSaved = config.readMidiMapFile();
        assert(callbacksSaved.chords[1].eventMask == 182);
        assert(!strcmp(callbacksSaved.chords[1].script, addon.c_str()));
        assert(midi.scripts.macroEventMasks[1] == 182);
        macroRequest("GET", "", scriptHexEncode(addon.c_str()).c_str());
        saveCallbacks(addon + "x", "400 Bad Request");
        assert(!strcmp(config.readMidiMapFile().chords[1].script, addon.c_str()));
        for (unsigned i = 1; i <= 2; ++i) {
            macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true, true);
            testNowMs += 20;
            midi.scripts.pollMacros(testNowMs);
            assert(hookState("count").integerValue == i);
            assert(!strcmp(hookState("kind").stringValue, "run") && !midi.scripts.macroErrors[1][0]);
        }
        ScriptEvent addonRelease{5, 66, 0, false};
        addonRelease.kind = ScriptEventKind::NoteOff;
        midi.scripts.dispatchMacroEvent(addonRelease);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("count").integerValue == 3 && !strcmp(hookState("kind").stringValue, "note_off"));
        addonRelease.kind = ScriptEventKind::ControlChange;
        midi.scripts.dispatchMacroEvent(addonRelease);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("count").integerValue == 4 && !strcmp(hookState("kind").stringValue, "cc"));
        ScriptEvent addonFinish{};
        addonFinish.kind = ScriptEventKind::SoundFinish;
        addonFinish.pressed = false;
        midi.scripts.dispatchMacroEvent(addonFinish);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("count").integerValue == 5 && !strcmp(hookState("kind").stringValue, "sound_finish"));
        midi.scripts.dispatchMacroEvent(connection);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("count").integerValue == 6 && !strcmp(hookState("kind").stringValue, "device_connect"));
        assert(!midi.scripts.macroErrors[1][0]);

        std::string configScript = "id=" + std::to_string(midiId) +
            "&name_hex=" + scriptHexEncode("Config hook") +
            "&midi_enabled=0&channel=0&note=0&keys=&script_hex=" +
            scriptHexEncode("config.write('volumes.txt','sfx=0.5\\n')");
        macroRequest("POST", configScript, "409 Conflict");
        macroRequest("POST", configScript + "&config_write_ack=2", "400 Bad Request");
        macroRequest("POST", configScript + "&config_write_ack=1", "200 OK");
        assert(config.readMidiMapFile().chords[1].configWriteAllowed);
        macroRequest("GET", "", "\"config_write_allowed\":true");
        assert(access("volumes.txt", F_OK) != 0);
        macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        char configText[128]{};
        size_t configLength = 0;
        assert(config.readScriptConfig("volumes.txt", configText, sizeof(configText), &configLength) == RET_OK);
        assert(!strcmp(configText, "sfx=0.5\n"));
        assert(!midi.scripts.macroErrors[1][0]);
        assert(unlink("volumes.txt") == 0);
        saveCallbacks("hook('boot',function(e) assert(e.source=='system'); "
                      "state.boots=(state.boots or 0)+1 end); "
                      "hook('timer',function(e) assert(e.source=='system'); "
                      "state.ticks=(state.ticks or 0)+1 end)", "200 OK", true);
        assert(config.readMidiMapFile().chords[1].eventMask == 768);
        initBootRecovery();
        pollBootRecovery();
        const uint64_t startupAt = testNowMs;
        midi.scripts.pollScriptLifecycle(testNowMs, bootStartupStable());
        assert(!midi.scripts.macroQueueCount && !midi.scripts.bootHooksDispatched);
        testNowMs = startupAt + (BOOT_RECOVERY_CONFIRM_SECONDS - 1) * 1000;
        pollBootRecovery();
        midi.scripts.pollScriptLifecycle(testNowMs, bootStartupStable());
        assert(!midi.scripts.macroQueueCount && !bootStartupStable());
        testNowMs = startupAt + BOOT_RECOVERY_CONFIRM_SECONDS * 1000;
        pollBootRecovery();
        assert(bootStartupStable());
        midi.scripts.pollScriptLifecycle(testNowMs, bootStartupStable());
        assert(midi.scripts.macroQueueCount == 1);
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("boots").integerValue == 1);
        midi.scripts.pollScriptLifecycle(testNowMs + 999, true);
        assert(!midi.scripts.macroQueueCount);
        testNowMs += 1000;
        midi.scripts.pollScriptLifecycle(testNowMs, true);
        assert(midi.scripts.macroQueueCount == 1);
        midi.scripts.pollScriptLifecycle(testNowMs + 1000, true);
        assert(midi.scripts.macroQueueCount == 1);
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("ticks").integerValue == 1 && hookState("boots").integerValue == 1);
        midi.scripts.pollScriptLifecycle(testNowMs + 5000, true);
        assert(midi.scripts.macroQueueCount == 1);
        testNowMs += 5000;
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("ticks").integerValue == 2 && hookState("boots").integerValue == 1);
        saveCallbacks("hook('boot',function() state.late=true end)");
        midi.scripts.pollScriptLifecycle(testNowMs + 1000, true);
        assert(!midi.scripts.macroQueueCount && midi.scripts.bootHooksDispatched);
        saveCallbacks("hook('run',function() system.restart() end)", "409 Conflict");
        saveCallbacks("hook('run',function() system.restart(); error('abort') end)", "200 OK", true);
        macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(scriptRestartRequests == 0 && strstr(midi.scripts.macroErrors[1], "abort"));
        saveCallbacks("hook('run',function() system.restart() end)", "200 OK", true);
        macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(scriptRestartRequests == 1 && strstr(midi.scripts.macroErrors[1], "system.restart failed"));
        saveCallbacks("hook('run',function() settings.set('midi_passthrough','unmapped') end)",
                      "200 OK", true);
        macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(config.readConfigFile().midiPassthrough == 1 && !midi.scripts.macroErrors[1][0]);
        {
            BluetoothContext bluetooth(&app);
            bluetooth.deviceCount = 1;
            bluetooth.devices[0] = {};
            assert(bt::parseAddress("AA:BB:CC:DD:EE:FF", &bluetooth.devices[0].address));
            strcpy(bluetooth.devices[0].name, "Test phone");
            bluetooth.devices[0].paired = true;
            memset(bluetooth.devices[0].linkKey, 0xA5, sizeof(bluetooth.devices[0].linkKey));
            saveCallbacks("hook('bluetooth_change',function(e) assert(e.source=='bluetooth'); "
                          "local b=bluetooth.devices(); assert(b.ready and #b.devices==1); "
                          "assert(b.devices[1].address=='AA:BB:CC:DD:EE:FF'); "
                          "assert(b.devices[1].linkKey==nil and b.devices[1].link_key==nil); "
                          "state.changes=(state.changes or 0)+1 end); "
                          "hook('run',function() bluetooth.connect('AA:BB:CC:DD:EE:FF') end)",
                          "200 OK", true);
            midi.scripts.pollScriptLifecycle(testNowMs, true);
            assert(!midi.scripts.macroQueueCount);
            bluetooth.scanning = true;
            midi.scripts.pollScriptLifecycle(testNowMs + 1, true);
            assert(midi.scripts.macroQueueCount == 1);
            testNowMs += 20;
            midi.scripts.pollMacros(testNowMs);
            assert(hookState("changes").integerValue == 1 && !midi.scripts.macroErrors[1][0]);
            midi.scripts.pollScriptLifecycle(testNowMs + 1, true);
            assert(!midi.scripts.macroQueueCount);
            macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
            testNowMs += 20;
            midi.scripts.pollMacros(testNowMs);
            assert(scriptBluetoothRequests == 1 && strstr(midi.scripts.macroErrors[1], "Bluetooth request rejected"));
            scriptBluetoothResult = RET_OK;
            macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
            testNowMs += 20;
            midi.scripts.pollMacros(testNowMs);
            assert(scriptBluetoothRequests == 2 && !midi.scripts.macroErrors[1][0]);
            saveCallbacks("hook('bluetooth_media_key',function(e) "
                          "assert(e.source=='bluetooth' and e.device=='AA:BB:CC:DD:EE:FF'); "
                          "assert(e.key=='pause'); state.presses=(state.presses or 0)+(e.pressed and 1 or 0); "
                          "state.releases=(state.releases or 0)+(e.pressed and 0 or 1) end)",
                          "200 OK", true);
            assert(config.readMidiMapFile().chords[1].eventMask == 8192);
            bluetooth.queueMediaEvent(bluetooth.devices[0].address, {0x46, true});
            bluetooth.queueMediaEvent(bluetooth.devices[0].address, {0x46, false});
            midi.scripts.pollBluetoothMediaEvents();
            assert(midi.scripts.macroQueueCount == 2 && !bluetooth.mediaEventCount);
            testNowMs += 20;
            midi.scripts.pollMacros(testNowMs);
            testNowMs += 20;
            midi.scripts.pollMacros(testNowMs);
            assert(hookState("presses").integerValue == 1);
            assert(hookState("releases").integerValue == 1 && !midi.scripts.macroErrors[1][0]);
            // Wraparound and overflow reporting preserve FIFO order.
            bluetooth.mediaEventHead = BluetoothContext::MEDIA_EVENTS_MAX - 1;
            for (unsigned i = 0; i < BluetoothContext::MEDIA_EVENTS_MAX + 3; ++i)
                bluetooth.queueMediaEvent(bluetooth.devices[0].address, {0x46, (i % 2) == 0});
            assert(bluetooth.mediaEventCount == BluetoothContext::MEDIA_EVENTS_MAX);
            assert(bluetooth.mediaEventsDropped == 3);
            BluetoothMediaEvent queued{};
            for (unsigned i = 0; i < BluetoothContext::MEDIA_EVENTS_MAX; ++i) {
                assert(bluetooth.popMediaEvent(queued));
                assert(queued.key.operation == 0x46 && queued.key.pressed == ((i % 2) == 0));
            }
            assert(!bluetooth.popMediaEvent(queued));
            bluetooth.queueMediaEvent(bluetooth.devices[0].address, {0x46, true});
            bluetooth.queueMediaEvent(bluetooth.devices[0].address, {0x46, false});
            midi.scripts.pollBluetoothMediaEvents();
            assert(midi.scripts.macroQueueCount == 2 && !bluetooth.mediaEventsDropped);
            midi.scripts.stopMacros();
            saveCallbacks("hook('bluetooth_media_key',function(e) "
                          "if e.pressed and e.key=='pause' then tap('MEDIA_PAUSE') end end)",
                          "200 OK", true);
            int mediaKeyboard[2];
            assert(socketpair(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK, 0, mediaKeyboard) == 0);
            const int previousMediaFd = midi.scripts.usb.mediaFd;
            const bool previousKeyboardEnabled = midi.scripts.usb.keyboardEnabled;
            const auto previousUsbMode = midi.scripts.usb.mode;
            const auto previousUsbProbeMs = midi.scripts.usb.nextProbeMs;
            midi.scripts.usb.mediaFd = mediaKeyboard[0];
            midi.scripts.usb.mode = 1;
            midi.scripts.usb.nextProbeMs = UINT64_MAX;
            midi.scripts.usb.keyboardEnabled = midi.keyboardEnabled = true;
            midi.scripts.macroUsbGeneration = midi.scripts.usb.sessionGeneration;
            const uint8_t pause[] = {0x10, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0x46, 0x00};
            const uint8_t pauseRelease[] = {0x20, 0x11, 0x0e, 0x00, 0x48, 0x7c, 0xc6, 0x00};
            for (const uint8_t *packet : {pause, pauseRelease}) {
                uint8_t response[8]{};
                bt::MediaKey key{};
                bool hasKey = false;
                assert(bt::avrcpResponse(packet, 8, response, sizeof(response), key, hasKey) == 8);
                assert(hasKey && response[3] == 0x09);
                bluetooth.queueMediaEvent(bluetooth.devices[0].address, key);
            }
            midi.scripts.pollBluetoothMediaEvents();
            testNowMs += 20;
            stepMacro(testNowMs);
            uint8_t report[3]{};
            assert(read(mediaKeyboard[1], report, sizeof(report)) == sizeof(report));
            assert(report[0] == SCRIPT_MEDIA_REPORT_ID && report[1] == 0xb1 && report[2] == 0);
            testNowMs += 30;
            stepMacro(testNowMs);
            assert(read(mediaKeyboard[1], report, sizeof(report)) == sizeof(report));
            assert(report[0] == SCRIPT_MEDIA_REPORT_ID && !report[1] && !report[2]);
            testNowMs += 20;
            stepMacro(testNowMs);
            assert(read(mediaKeyboard[1], report, sizeof(report)) < 0 && errno == EAGAIN);
            testNowMs += 20;
            stepMacro(testNowMs);
            assert(read(mediaKeyboard[1], report, sizeof(report)) < 0 && errno == EAGAIN);
            assert(!midi.scripts.macroErrors[1][0] && !midi.scripts.macroQueueCount);
            midi.scripts.stopMacros();
            close(mediaKeyboard[0]);
            close(mediaKeyboard[1]);
            midi.scripts.usb.mediaFd = previousMediaFd;
            midi.scripts.usb.keyboardEnabled = previousKeyboardEnabled;
            midi.scripts.usb.mode = previousUsbMode;
            midi.scripts.usb.nextProbeMs = previousUsbProbeMs;
            midi.keyboardEnabled = false;
        }
        saveCallbacks(
            "ui.option('enabled',{label='Enabled',type='boolean',default=true}); "
            "ui.option('level',{label='Level',type='number',default=50,min=0,max=100}); "
            "ui.option('name',{label='Name',type='string',default='default'}); "
            "ui.button('apply','<Apply>'); "
            "hook('config_change',function(e) state.key=e.key; state.value=e.value; "
            "assert(script_config.get(e.key)==e.value) end); "
            "hook('ui',function(e) assert(e.key=='apply' and e.value==nil); state.applied=true end); "
            "hook('run',function() storage.write('private.bin','A\\0B'); "
            "script_config.set('starts',script_config.get('starts',0)+1) end)");
        macroRequest("GET", "", "\"type\":\"number\",\"value\":50", false, true);
        auto controlRequest = [&](const std::string &body, const char *expected) {
            int sockets[2];
            assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
            assert(handleScriptControl(&http, sockets[0], "POST", body.data(), body.size()) == 0);
            assert(shutdown(sockets[0], SHUT_WR) == 0);
            std::string response;
            char buffer[2048];
            ssize_t got;
            while ((got = read(sockets[1], buffer, sizeof(buffer))) > 0)
                response.append(buffer, static_cast<size_t>(got));
            if (response.find(expected) == std::string::npos)
                fprintf(stderr, "Expected %s, got %s\n", expected, response.c_str());
            assert(response.find(expected) != std::string::npos);
            close(sockets[0]);
            close(sockets[1]);
        };
        const std::string controlId = "id=" + std::to_string(midiId);
        auto option = [&](const char *key, const char *value, const char *status = "200 OK") {
            controlRequest(controlId + "&key_hex=" + scriptHexEncode(key) +
                           "&value_hex=" + scriptHexEncode(value), status);
        };
        option("level", "70");
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("value").numberValue == 70 && !midi.scripts.macroErrors[1][0]);
        option("enabled", "false");
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(!hookState("value").booleanValue);
        option("name", "<unsafe>&text");
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(!strcmp(hookState("value").stringValue, "<unsafe>&text"));
        macroRequest("GET", "", "\"type\":\"number\",\"value\":70", false, true);
        option("level", "101", "400 Bad Request");
        option("level", "nan", "400 Bad Request");
        option("enabled", "1", "400 Bad Request");
        option("unknown", "x", "404 Not Found");
        controlRequest(controlId + "&key_hex=" + scriptHexEncode("apply"), "button queued");
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(hookState("applied").booleanValue && !midi.scripts.macroErrors[1][0]);
        controlRequest(controlId + "&key_hex=" + scriptHexEncode("apply") + "&value_hex=31",
                       "400 Bad Request");
        controlRequest(controlId + "&id=1&key_hex=31", "400 Bad Request");
        const std::string dataDirectory = "scripts/" + std::to_string(midiId);
        ScriptStorage privateStorage(midiId);
        ScriptState savedOptions{};
        assert(privateStorage.readSettings(savedOptions) == RET_OK && savedOptions.count == 3);
        assert(access((dataDirectory + "/private.bin").c_str(), F_OK) != 0);
        macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        std::string binary;
        assert(privateStorage.readFile("private.bin", binary) == RET_OK && binary == std::string("A\0B", 3));
        assert(privateStorage.readSettings(savedOptions) == RET_OK && savedOptions.count == 4);
        const uint32_t orphanId = saveNamed("Temporary storage owner", "log('temporary')");
        ScriptStorage orphan(orphanId);
        assert(orphan.writeFile("owned.txt", "private", 7) == RET_OK);
        macroRequest("DELETE", "id=" + std::to_string(orphanId), "200 OK");
        const uint32_t replacementId = saveNamed("New isolated script", "log('replacement')");
        assert(replacementId > orphanId);
        ScriptStorage replacement(replacementId);
        std::string isolated;
        assert(replacement.readFile("owned.txt", isolated) == RET_ERR && errno == ENOENT);
        macroRequest("DELETE", "id=" + std::to_string(replacementId), "200 OK");
        assert(orphan.removeFile("owned.txt") == RET_OK);
        assert(rmdir(("scripts/" + std::to_string(orphanId)).c_str()) == 0);
        for (unsigned i = 0; i < SCRIPTS_MAX; ++i)
            controlRequest(controlId + "&key_hex=" + scriptHexEncode("apply"), "200 OK");
        option("level", "80", "409 Conflict");
        assert(privateStorage.readSettings(savedOptions) == RET_OK);
        for (unsigned i = 0; i < savedOptions.count; ++i)
            if (!strcmp(savedOptions.entries[i].key, "level")) assert(savedOptions.entries[i].numberValue == 70);
        midi.scripts.stopMacros();
        const std::string settingsPath = dataDirectory + "/.settings";
        assert(unlink(settingsPath.c_str()) == 0);
        assert(mkdir(settingsPath.c_str(), 0700) == 0);
        option("level", "80", "500 Internal Server Error");
        assert(!midi.scripts.macroQueueCount);
        macroRequest("GET", "", "\"ui_error_hex\":", false, true);
        assert(rmdir(settingsPath.c_str()) == 0);
        ScriptStateEntry restored{};
        strcpy(restored.key, "level");
        restored.kind = ScriptValueKind::Number;
        restored.numberValue = 70;
        assert(privateStorage.setSetting(restored) == RET_OK);
        saveCallbacks("hook('run',function() assert(script_config.get('level')==70); "
                      "assert(storage.read('private.bin')=='A\\0B') end)");
        macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(!midi.scripts.macroErrors[1][0]);
        saveCallbacks("hook('run',function() storage.write('abort.bin','bad'); "
                      "script_config.set('level',80); error('abort storage') end)");
        macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(strstr(midi.scripts.macroErrors[1], "abort storage"));
        assert(privateStorage.readFile("abort.bin", binary) == RET_ERR && errno == ENOENT);
        assert(privateStorage.readSettings(savedOptions) == RET_OK);
        for (unsigned i = 0; i < savedOptions.count; ++i)
            if (!strcmp(savedOptions.entries[i].key, "level")) assert(savedOptions.entries[i].numberValue == 70);
        saveCallbacks("hook('run',function() storage.remove('private.bin') end)");
        macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        testNowMs += 20;
        midi.scripts.pollMacros(testNowMs);
        assert(!midi.scripts.macroErrors[1][0] && privateStorage.readFile("private.bin", binary) == RET_ERR);
        assert(unlink((dataDirectory + "/.settings").c_str()) == 0);
        assert(rmdir(dataDirectory.c_str()) == 0);
        assert(rmdir("scripts") == 0);
        hookScript(1, "state.count=(state.count or 0)+1");
        for (unsigned i = 0; i < SCRIPTS_MAX; ++i)
            macroRequest("POST", "id=" + std::to_string(midiId), "200 OK", true);
        macroRequest("POST", "id=" + std::to_string(midiId), "409 Conflict", true);
        midi.disconnect();
        close(midiInput[1]);
        close(usbKeyboard[1]);
        testClockEnabled = false;
    }

    {
        char configText[SCRIPT_CONFIG_READ_MAX + 1]{};
        size_t configLength = 0;
        assert(config.readScriptConfig("bluetooth.txt", configText, sizeof(configText), &configLength) == RET_ERR);
        assert(config.readScriptConfig("../config.txt", configText, sizeof(configText), &configLength) == RET_ERR);
        assert(config.readScriptConfig("volumes.txt", configText, sizeof(configText), &configLength) == RET_ERR);
        assert(symlink("config.txt", "volumes.txt") == 0);
        assert(config.readScriptConfig("volumes.txt", configText, sizeof(configText), &configLength) == RET_ERR);
        assert(unlink("volumes.txt") == 0);
        assert(mkdir("volumes.txt", 0700) == 0);
        assert(config.writeScriptConfig("volumes.txt", "sfx=1\n", 6) == RET_ERR);
        assert(rmdir("volumes.txt") == 0);
        assert(config.writeScriptConfig("volumes.txt", "sfx=1\n", 6) == RET_OK);
        ScriptState state{};
        ScriptPlan plan{};
        char error[256]{};
        ScriptEvent event{};
        assert(scriptEvaluate("local s=settings.get(); assert(s.midi_passthrough=='unmapped'); "
                                  "assert(value_type(s.keyboard_macros)=='boolean')",
                                  event, state, plan, error, sizeof(error), &config));
        assert(scriptEvaluate("settings.set('usb_capture_channels',16)",
                                  event, state, plan, error, sizeof(error), &config, true));
        assert(plan.count == 1 && plan.actions[0].kind == ScriptKind::SettingsSet &&
               plan.actions[0].setting.numberValue == 16);
        for (const char *invalid : {
                "settings.set('usb_capture_channels',17)", "settings.set('usb_sample_size',0)",
                "settings.set('keyboard_macros',1)", "settings.set('midi_passthrough','invalid')",
                "settings.set('unknown',true)", "bluetooth.connect('../config.txt')",
                "bluetooth.auto_connect('AA:BB:CC:DD:EE:FF',1)",
                "bluetooth.connect('00:00:00:00:00:00')", "system.logs('config.txt')",
                "system.logs('stdout',129)", "system.restart(1)"}) {
            assert(!scriptEvaluate(invalid, event, state, plan, error, sizeof(error), &config, true));
            assert(error[0] && plan.count == 0);
        }
        assert(!scriptEvaluate("settings.set('keyboard_macros',true)",
                                   event, state, plan, error, sizeof(error), &config));
        FILE *log = fopen("stdout.log", "w");
        assert(log && fputs("first\nsecond\nlast\n", log) >= 0 && fclose(log) == 0);
        assert(scriptEvaluate("assert(system.logs('stdout',2)=='second\\nlast\\n')",
                                  event, state, plan, error, sizeof(error), &config));
        assert(!scriptEvaluate("for i=1,5 do system.logs() end",
                                   event, state, plan, error, sizeof(error), &config));
        assert(strstr(error, "read limit"));
        assert(!scriptEvaluate("system.logs('stderr')",
                                   event, state, plan, error, sizeof(error), &config));
        assert(unlink("stdout.log") == 0);
        assert(scriptEvaluate("assert(config.read('volumes.txt')=='sfx=1\\n'); "
                                  "assert(config.lines('volumes.txt')[1]=='sfx=1')",
                                  event, state, plan, error, sizeof(error), &config));
        assert(!scriptEvaluate("for i=1,5 do config.read('volumes.txt') end",
                                   event, state, plan, error, sizeof(error), &config));
        assert(strstr(error, "read limit"));
        assert(!scriptEvaluate("config.write('volumes.txt','sfx=0\\n'); error('abort')",
                                   event, state, plan, error, sizeof(error), &config, true));
        assert(config.readScriptConfig("volumes.txt", configText, sizeof(configText), &configLength) == RET_OK);
        assert(!strcmp(configText, "sfx=1\n"));
        FILE *fp = fopen("volumes.txt", "w");
        assert(fp);
        std::string line = std::string(511, 'x') + "\n";
        for (unsigned i = 0; i < 128; ++i)
            assert(fwrite(line.data(), 1, line.size(), fp) == line.size());
        assert(fclose(fp) == 0);
        assert(config.readScriptConfig("volumes.txt", configText, sizeof(configText), &configLength) == RET_OK);
        assert(configLength == SCRIPT_CONFIG_READ_MAX);
        assert(scriptEvaluate("assert(#config.read('volumes.txt')==65536)",
                                  event, state, plan, error, sizeof(error), &config));
        assert(scriptEvaluate("local lines=config.lines('volumes.txt'); assert(#lines==128 and #lines[1]==511)",
                                  event, state, plan, error, sizeof(error), &config));
        fp = fopen("volumes.txt", "w");
        assert(fp);
        std::string oversized(SCRIPT_CONFIG_READ_MAX + 1, 'x');
        assert(fwrite(oversized.data(), 1, oversized.size(), fp) == oversized.size());
        assert(fclose(fp) == 0);
        assert(config.readScriptConfig("volumes.txt", configText, sizeof(configText), &configLength) == RET_ERR);
        fp = fopen("volumes.txt", "w");
        assert(fp && fwrite("x\0y", 1, 3, fp) == 3);
        assert(fclose(fp) == 0);
        assert(config.readScriptConfig("volumes.txt", configText, sizeof(configText), &configLength) == RET_ERR);
        assert(unlink("volumes.txt") == 0);
    }

    {
        const std::string futureConfig = "config_version=2\nusb_sample_rate=96000\n";
        FILE *future = fopen("config.txt", "w");
        assert(future);
        assert(fwrite(futureConfig.data(), 1, futureConfig.size(), future) == futureConfig.size());
        assert(fclose(future) == 0);
        ConfigData unsupported = config.readConfigFile();
        assert(unsupported.schemaVersion == 2);
        assert(config.writeConfigFile(&unsupported) == RET_ERR);
        std::ifstream preserved("config.txt");
        assert(preserved);
        assert(std::string(std::istreambuf_iterator<char>(preserved), {}) == futureConfig);
    }

    {
        const std::string futureMap = "# audiox midi map v2\nnote_map=36,future.wav\n";
        FILE *future = fopen("midi_map.txt", "w");
        assert(future);
        assert(fwrite(futureMap.data(), 1, futureMap.size(), future) == futureMap.size());
        assert(fclose(future) == 0);
        MidiMapData emptyMap{};
        assert(config.writeMidiMapFile(&emptyMap) == RET_ERR);
        assert(readMidiConfig() == futureMap);
        assert(unlink("midi_map.txt") == 0);

        assert(config.writeMidiMapFile(&emptyMap) == RET_OK);
        const std::string savedMap = readMidiConfig();
        const std::string futureParams = "# audiox fx params v2\nfx_param=fx_1,gain,0.5\n";
        future = fopen("fx_params.txt", "w");
        assert(future);
        assert(fwrite(futureParams.data(), 1, futureParams.size(), future) == futureParams.size());
        assert(fclose(future) == 0);
        assert(config.writeMidiMapFile(&emptyMap) == RET_ERR);
        assert(readMidiConfig() == savedMap);
        std::ifstream preservedParams("fx_params.txt");
        assert(preservedParams);
        assert(std::string(std::istreambuf_iterator<char>(preservedParams), {}) == futureParams);
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
