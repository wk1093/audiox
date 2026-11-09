#include "config/scenes.hpp"
#include "audio/scene_fade.hpp"
#include "http/scenes.hpp"
#include "config/backup.hpp"
#include "update_archive.hpp"
#include "../script_device_stubs.hpp"
#include <cassert>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sys/socket.h>
#include <unistd.h>

size_t BluetoothContext::copyRoutingDevices(BluetoothRoutingDevice *, size_t) const { return 0; }
static bool failSync = false;
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (failSync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}

namespace {
void write(const char *path, const std::string &text) {
    std::ofstream stream(path);
    stream << text;
    stream.close();
    assert(stream.good());
}
std::string read(const char *path) {
    std::ifstream stream(path);
    return std::string(std::istreambuf_iterator<char>(stream), {});
}
std::string request(HttpServer &server, const char *method, const char *path, const char *query = nullptr) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(handleSceneRequest(&server, sockets[0], method, path, query) == 0);
    assert(shutdown(sockets[0], SHUT_WR) == 0);
    std::string result;
    char buffer[4096];
    ssize_t size;
    while ((size = ::read(sockets[1], buffer, sizeof(buffer))) > 0) result.append(buffer, size);
    close(sockets[0]);
    close(sockets[1]);
    return result;
}
}
int main() {
    namespace fs = std::filesystem;
    using namespace audiox::scenes;
    const fs::path original = fs::current_path();
    const fs::path scratch = original / "out" / "host" / ("scenes-test-" + std::to_string(getpid()));
    assert(fs::create_directories(scratch));
    assert(chdir(scratch.c_str()) == 0);
    assert(fs::create_directory("config"));
    {
        Audiox app;
        ConfigStore config(&app);
        AudioContext audio(&app);
        HttpServer server(&app);
        app.config = &config;
        app.audio = &audio;
        app.http = &server;
        write("config/routing.txt", "# audiox routing v1\nedge=soundboard_out,fx_1,0,0\n");
        write("config/config.txt", "# audiox config v1\nsample_rate=48000\n");
        const std::string oldConfig = read("config/config.txt");
        auto map = config.readMidiMapFile();
        map.effectStateCount = 1;
        snprintf(map.effectStates[0].effectId, 64, "fx_1");
        snprintf(map.effectStates[0].type, 24, "gain");
        snprintf(map.effectStates[0].name, 64, "Clean gain");
        map.effectStates[0].enabled = 1;
        map.effectStates[0].gain = 0.25f;
        map.effectStates[0].drive = map.effectStates[0].clip = map.effectStates[0].output = 1;
        assert(config.writeMidiMapFile(&map) == RET_OK);
        audio.loadEffectsFromConfig();
        audio.setThingGain("soundboard_out", 0.75f);
        // Live MIDI/HTTP changes must win over the older persisted parameter.
        assert(audio.setEffectParam("fx_1", "gain", 0.6f) == RET_OK);
        Scene clean;
        clean.id = "clean";
        clean.name = "Clean \"voice\"";
        clean.channel = 2;
        clean.note = 60;
        std::string error;
        assert(capture(&app, clean, error));
        assert(clean.effects.size() == 1 && clean.params[0].value == 0.6f);
        Store store;
        assert(store.save(clean, error));
        Scene loaded;
        assert(Store().read("clean", loaded, error));
        assert(loaded.name == clean.name && loaded.routes == clean.routes);
        assert(loaded.volumes[0].gain == 0.75f);
        assert(loaded.effects[0].enabled == 1 && loaded.params[0].value == 0.6f);
        const std::string disk = read("config/scenes/clean.scene");
        assert(disk.find("audiox-scenes 1\n") == 0);
        failSync = true;
        Scene replacement = clean;
        replacement.name = "Failed replacement";
        assert(!store.save(replacement, error));
        failSync = false;
        assert(read("config/scenes/clean.scene") == disk);
        assert(!fs::exists("config/scenes/clean.scene.pending"));
        Scene invalid = clean;
        invalid.id = "../escape";
        assert(!store.save(invalid, error));
        invalid = clean;
        invalid.volumes[0].gain = std::numeric_limits<float>::quiet_NaN();
        assert(!store.save(invalid, error));
        invalid = clean;
        invalid.params.clear();
        assert(!store.save(invalid, error));
        invalid = clean;
        invalid.id = "duplicate";
        assert(!store.save(invalid, error));
        invalid.name = "Unique";
        assert(!store.save(invalid, error)); // duplicate MIDI binding
        std::string decoded;
        assert(decode(encode("Unicode café"), decoded) && decoded == "Unicode café");
        assert(!decode("00", decoded) && !decode("zz", decoded));
        assert(request(server, "GET", "/api/scenes").find("Clean \\\"voice\\\"") != std::string::npos);
        assert(request(server, "PUT", "/api/scenes").find("405") != std::string::npos);
        assert(request(server, "POST", "/api/scenes/recall", "id=../escape").find("400") != std::string::npos);
        assert(request(server, "POST", "/api/scenes/bind", "id=clean&channel=16&note=60").find("400") != std::string::npos);
        Scene missingPlugin = clean;
        missingPlugin.id = "unavailable";
        missingPlugin.name = "Unavailable plugin";
        missingPlugin.channel = missingPlugin.note = -1;
        snprintf(missingPlugin.effects[0].type, 24, "plugin_uninstalled");
        assert(store.save(missingPlugin, error));
        assert(store.read(missingPlugin.id, loaded, error));
        assert(!recall(&app, missingPlugin, error));
        assert(audio.effectStates.size() == 1 && audio.getThingGain("soundboard_out") == 0.75f);
        assert(store.remove(missingPlugin.id, error));
        // A stalled processing thread must not cause a non-faded setup swap.
        audio.processingThreadStarted = 1;
        audio.processingThreadRun.store(1);
        assert(!recall(&app, clean, error));
        assert(error.find("timed out") != std::string::npos);
        audio.processingThreadStarted = 0;
        audio.processingThreadRun.store(0);
        assert(!audio.sceneMuted.load() && read("config/config.txt") == oldConfig);

        char pitchId[64];
        assert(audio.createEffect("pitch", pitchId, sizeof(pitchId)) == RET_OK);
        assert(audio.setEffectParam(pitchId, "quality", 1.6f) == RET_OK);
        Scene pitchScene;
        pitchScene.id = "pitch-quality";
        pitchScene.name = "Pitch quality";
        assert(capture(&app, pitchScene, error));
        auto quality = std::find_if(pitchScene.params.begin(), pitchScene.params.end(), [&](const auto &p) {
            return !strcmp(p.effectId, pitchId) && !strcmp(p.param, "quality");
        });
        assert(quality != pitchScene.params.end() && quality->value == 2);
        assert(store.save(pitchScene, error));
        assert(store.read(pitchScene.id, loaded, error));
        assert(recall(&app, loaded, error));
        audiox::effects::SlotParams pitchParams{};
        assert(audio.getEffectParams(pitchId, &pitchParams) == RET_OK && pitchParams.values[6] == 2);
        pitchScene.params.erase(quality);
        assert(store.save(pitchScene, error));
        assert(store.read(pitchScene.id, loaded, error));
        assert(recall(&app, loaded, error));
        assert(audio.getEffectParams(pitchId, &pitchParams) == RET_OK && pitchParams.values[6] == 0);
        assert(recall(&app, clean, error));
        assert(store.remove(pitchScene.id, error));

        char secondId[64];
        assert(audio.createEffect("distortion", secondId, sizeof(secondId)) == RET_OK);
        assert(audio.setEffectParam(secondId, "drive", 4.0f) == RET_OK);
        audio.setThingGain("soundboard_out", 0.2f);
        audio.setThingGain("orphan_gain", 1.5f);
        auto router = config.router();
        assert(router.replaceSceneRoutes({"soundboard_out," + std::string(secondId) + ",0,0"}, true) == RET_OK);
        Scene live;
        live.id = "live";
        live.name = "Live";
        assert(capture(&app, live, error));
        assert(live.effects.size() == 2 && live.helper);
        assert(store.save(live, error));
        // Backup carries the scene inventory, but not interrupted writes or notes.
        write("config/scenes/clean.scene.pending", "incomplete");
        write("config/scenes/notes.txt", "not scene configuration");
        char backupError[256]{};
        int archive = open("backup.axb", O_RDWR | O_CREAT | O_TRUNC, 0600);
        assert(archive >= 0);
        assert(audiox::backup::create(".", archive, backupError, sizeof(backupError)) == 0);
        const std::string archiveText = read("backup.axb");
        assert(archiveText.find("scenes/clean.scene") != std::string::npos);
        assert(archiveText.find("scenes/live.scene") != std::string::npos);
        assert(archiveText.find("scenes/clean.scene.pending") == std::string::npos);
        assert(archiveText.find("scenes/notes.txt") == std::string::npos);
        assert(store.remove("clean", error));
        assert(audiox::backup::stage(".", archive, archiveText.size(), backupError, sizeof(backupError)) == 0);
        assert(audiox::backup::applyPending(".", backupError, sizeof(backupError)) == 0);
        assert(store.read("clean", loaded, error) && loaded.name == clean.name);
        assert(!fs::exists("config/scenes/notes.txt"));
        std::string invalidArchive = archiveText;
        const size_t marker = invalidArchive.find("audiox-scenes 1\n");
        assert(marker != std::string::npos);
        invalidArchive[marker + strlen("audiox-scenes ")] = '9';
        const uint32_t crc = audioxUpdateCrc32(0, invalidArchive.data(), invalidArchive.size() - 4);
        audioxUpdatePutU32(reinterpret_cast<unsigned char *>(invalidArchive.data() + invalidArchive.size() - 4), crc);
        assert(pwrite(archive, invalidArchive.data(), invalidArchive.size(), 0) == static_cast<ssize_t>(invalidArchive.size()));
        assert(audiox::backup::stage(".", archive, invalidArchive.size(), backupError, sizeof(backupError)) < 0);
        assert(strstr(backupError, "scene schema"));
        close(archive);
        assert(recall(&app, clean, error));
        assert(audio.effectStates.size() == 1);
        assert(audio.effectStates.count("fx_1") == 1);
        assert(audio.getThingGain("soundboard_out") == 0.75f);
        assert(audio.getThingGain("orphan_gain") == 1.0f);
        assert(!router.hasRoutingHelper());
        audiox::effects::SlotParams params{};
        assert(audio.getEffectParams("fx_1", &params) == RET_OK && params.values[0] == 0.6f);
        assert(read("config/config.txt") == oldConfig);
        assert(recall(&app, live, error));
        assert(audio.effectStates.size() == 2 && router.hasRoutingHelper());
        assert(audio.getThingGain("orphan_gain") == 1.5f);
        assert(recallMidi(&app, 2, 60));
        assert(audio.effectStates.size() == 1 && audio.activeSceneId == "clean");
        assert(!recallMidi(&app, 1, 60));
        assert(request(server, "POST", "/api/scenes/bind", "id=clean&channel=3&note=61").find("200") != std::string::npos);
        assert(!recallMidi(&app, 2, 60));
        assert(recallMidi(&app, 3, 61));
        assert(request(server, "POST", "/api/scenes/save", "id=captured&name_hex=4361707475726564&channel=-1&note=-1").find("200") != std::string::npos);
        assert(request(server, "DELETE", "/api/scenes/delete", "id=captured").find("200") != std::string::npos);
        // Unsupported versions are not silently migrated or overwritten.
        write("config/scenes/clean.scene", "audiox-scenes 999\n");
        assert(!store.read("clean", loaded, error));
        assert(!store.save(clean, error));
        assert(read("config/scenes/clean.scene") == "audiox-scenes 999\n");
        archive = open("invalid-backup.axb", O_RDWR | O_CREAT | O_TRUNC, 0600);
        assert(archive >= 0);
        assert(audiox::backup::create(".", archive, backupError, sizeof(backupError)) < 0);
        assert(strstr(backupError, "scene schema"));
        close(archive);
        write("config/scenes/clean.scene", disk);
        assert(store.remove("clean", error));
        assert(!store.read("clean", loaded, error));
        std::vector<Scene> scenes;
        assert(store.list(scenes, error) && scenes.size() == 1);
        // Per-sample transitions are bounded, reach exact silence and unity.
        Fade fade;
        float previous = 1;
        for (unsigned i = 0; i < 480; ++i) {
            float next = fade.next(true, 48000);
            assert(next <= previous && previous - next <= 1.0f / 480 + 1e-6f);
            previous = next;
        }
        for (unsigned i = 0; i < 4; ++i) fade.next(true, 48000);
        assert(fade.gain == 0);
        for (unsigned i = 0; i < 484; ++i) fade.next(false, 48000);
        assert(fade.gain == 1);
        puts("scene storage, inventory recall, MIDI, API and fade tests passed");
    }
    assert(chdir(original.c_str()) == 0);
    fs::remove_all(scratch);
}
