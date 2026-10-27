#include "audio/context.hpp"
#include "config/context.hpp"
#include "midi/context.hpp"
#include "touch/context.hpp"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <linux/input.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../src/http/api.cpp"

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
        int leds[2];
        assert(pipe(leds) == 0);
        midi.fd = leds[1];
        midi.connected = 1;
        midi.refreshAllLighting();
        expectLights(leds[0], 11, 15, 12);

        midi.heldNote = 36;
        midi.holdActive = 1;
        midi.samplerSelectedValid = 1;
        strcpy(midi.samplerSelectedSfx, "remove.wav");
        HttpServer http(&app);
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
    }

    assert(unlink("sfx/keep.wav") == 0);
    assert(rmdir("sfx") == 0);
    assert(unlink("config.txt") == 0);
    assert(unlink("midi_map.txt") == 0);
    assert(unlink("fx_params.txt") == 0);
    assert(fchdir(cwd) == 0);
    close(cwd);
    assert(rmdir(temp) == 0);
    puts("soundboard cleanup, MIDI lighting, and Cut persistence tests passed");
}
