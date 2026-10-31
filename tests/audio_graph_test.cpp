#include "../src/http/api.cpp"

#include <cassert>
#include <dirent.h>
#include <fstream>
#include <limits>
#include <sys/stat.h>
#include <sys/socket.h>

namespace {
bool failSync = false;
bool failClose = false;
bool failScan = false;

std::string readFile(const char *path) {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

void writeFile(const char *path, const std::string &text) {
    std::ofstream output(path);
    output << text;
    output.close();
    assert(output.good());
}

template<typename Handler>
std::string request(Handler handler) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(handler(sockets[0]) == 0);
    assert(shutdown(sockets[0], SHUT_WR) == 0);
    std::string response;
    char buffer[4096];
    ssize_t size;
    while ((size = read(sockets[1], buffer, sizeof(buffer))) > 0) {
        response.append(buffer, static_cast<size_t>(size));
    }
    assert(size == 0);
    close(sockets[0]);
    close(sockets[1]);
    return response;
}
}

extern "C" DIR *__real_opendir(const char *);
extern "C" DIR *__wrap_opendir(const char *path) {
    if (strcmp(path, "/dev/snd") == 0) {
        if (failScan) { errno = EACCES; return nullptr; }
        return __real_opendir("snd");
    }
    return __real_opendir(path);
}
extern "C" int __real_stat(const char *, struct stat *);
extern "C" int __wrap_stat(const char *path, struct stat *info) {
    if (strncmp(path, "/dev/snd/", 9) == 0) {
        std::string local = std::string("snd/") + (path + 9);
        return __real_stat(local.c_str(), info);
    }
    return __real_stat(path, info);
}
extern "C" int __real_fsync(int);
extern "C" int __wrap_fsync(int fd) {
    if (failSync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
extern "C" int __real_close(int);
extern "C" int __wrap_close(int fd) {
    const int result = __real_close(fd);
    if (failClose) { failClose = false; errno = EIO; return -1; }
    return result;
}

size_t BluetoothContext::copyRoutingDevices(BluetoothRoutingDevice *, size_t) const { return 0; }

int main() {
    char original[4096];
    assert(getcwd(original, sizeof(original)));
    char directory[] = "/tmp/audiox-graph-XXXXXX";
    assert(mkdtemp(directory));
    assert(chdir(directory) == 0);
    writeFile("midi_map.txt", "");
    writeFile("fx_params.txt", "");
    {
        Audiox app;
        ConfigStore config(&app);
        app.config = &config;
        auto audio = std::make_unique<AudioContext>(&app);
        app.audio = audio.get();
        HttpServer http(&app);
        assert(audio->reloadRoutingGraph() == RET_OK);
        const auto initial = audio->routingGraph.generation;
        audio->poll();
        assert(audio->routingGraph.generation == initial);

        assert(mkdir("snd", 0700) == 0);
        writeFile("snd/pcmC98D0p", "");
        writeFile("routing.txt", "edge=soundboard_out,alsa_card98_dev0_out,0,0\n");
        audio->nextHotplugScanMs = 0;
        audio->poll();
        assert(audio->devices.size() == 1);
        assert(audio->routingGraph.generation > initial);
        assert(audio->routingGraph.edgeCount == 1);
        AudioGraphState delivered{};
        assert(audio->routingGraphMailbox.consume(&delivered));
        assert(memcmp(&delivered, &audio->routingGraph, sizeof(delivered)) == 0);
        const auto connected = audio->routingGraph.generation;
        const auto topology = audio->deviceGeneration.load();
        audio->nextHotplugScanMs = 0;
        audio->poll();
        assert(audio->routingGraph.generation == connected);
        assert(audio->deviceGeneration == topology);
        assert(!audio->routingGraphMailbox.consume(&delivered));
        failScan = true;
        audio->nextHotplugScanMs = 0;
        audio->poll();
        failScan = false;
        assert(audio->routingGraph.generation == connected && audio->devices.size() == 1);

        // All device-name surfaces must escape quotes, backslashes and controls.
        strcpy(audio->devices.begin()->second.displayName, "USB \"Studio\"\\\n\t\001");
        const std::string escaped = "USB \\\"Studio\\\"\\\\\\u000a\\u0009\\u0001";
        const auto things = request([&](int fd) { return handleRoutingThings(&http, fd, "GET", ""); });
        assert(things.find(escaped + " Out") != std::string::npos);
        const auto volumes = request([&](int fd) { return handleAudioVolumes(&http, fd, "GET", ""); });
        assert(volumes.find(escaped + " Out") != std::string::npos);
        char devices[4096];
        assert(audio->buildDevicesJson(devices, sizeof(devices)) == RET_OK);
        assert(std::string(devices).find(escaped) != std::string::npos);
        assert(audio->buildDevicesJson(devices, 8) == RET_ERR);
        assert(audiox::json::escape("\b\f\r") == "\\u0008\\u000c\\u000d");
        assert(audiox::json::escape("caf\xc3\xa9") == "caf\xc3\xa9");

        assert(unlink("snd/pcmC98D0p") == 0);
        audio->nextHotplugScanMs = 0;
        audio->poll();
        assert(audio->devices.empty());
        assert(audio->routingGraph.generation > connected);
        assert(audio->routingGraph.edgeCount == 0);
        assert(readFile("routing.txt").find("alsa_card98_dev0_out") != std::string::npos);
        assert(rmdir("snd") == 0);
        const auto removed = audio->routingGraph.generation;
        audio->nextHotplugScanMs = 0;
        audio->poll();
        assert(audio->routingGraph.generation == removed);

        auto setVolume = [&](const std::string &body) {
            return request([&](int fd) { return handleAudioVolumeSet(&http, fd, "POST", "", body.data(), body.size()); });
        };
        assert(setVolume("id=soundboard_out&gain=0.5").find("200 OK") != std::string::npos);
        assert(audio->getThingGain("soundboard_out") == 0.5f);
        const auto saved = readFile("volumes.txt");
        for (const char *invalid : {"nan", "inf", "-inf", "0.5junk", "", "1e999"}) {
            assert(setVolume(std::string("id=soundboard_out&gain=") + invalid).find("400 Bad Request") != std::string::npos);
            assert(audio->getThingGain("soundboard_out") == 0.5f);
            assert(readFile("volumes.txt") == saved);
        }
        audio->setThingGain("soundboard_out", std::numeric_limits<float>::quiet_NaN());
        assert(audio->getThingGain("soundboard_out") == 0.5f);
        for (bool closeFailure : {false, true}) {
            failSync = !closeFailure;
            failClose = closeFailure;
            assert(setVolume("id=soundboard_out&gain=0.8").find("500 Internal Server Error") != std::string::npos);
            failSync = false;
            assert(readFile("volumes.txt") == saved);
            assert(audio->getThingGain("soundboard_out") == 0.5f);
        }
        assert(unlink("volumes.txt") == 0);
        assert(mkdir("volumes.txt", 0700) == 0);
        VolumeEntry entry{};
        strcpy(entry.thingId, "soundboard_out");
        entry.gain = 0.8f;
        assert(config.writeVolumesFile(&entry, 1) == RET_ERR); // rename failure
        assert(setVolume("id=soundboard_out&gain=0.8").find("500 Internal Server Error") != std::string::npos);
        assert(audio->getThingGain("soundboard_out") == 0.5f);
        assert(rmdir("volumes.txt") == 0);
        for (const char *invalid : {"nan", "inf", "0.5junk"}) {
            writeFile("volumes.txt", std::string("soundboard_out=") + invalid + "\n");
            VolumeEntry loaded[VOLUME_ENTRIES_MAX];
            uint32_t count = 0;
            assert(config.readVolumesFile(loaded, &count, VOLUME_ENTRIES_MAX) == RET_ERR);
        }
        entry.gain = std::numeric_limits<float>::infinity();
        const auto corrupt = readFile("volumes.txt");
        assert(config.writeVolumesFile(&entry, 1) == RET_ERR);
        assert(readFile("volumes.txt") == corrupt);
        std::string full;
        for (unsigned i = 0; i < VOLUME_ENTRIES_MAX; ++i) full += "device_" + std::to_string(i) + "=1\n";
        writeFile("volumes.txt", full);
        assert(setVolume("id=soundboard_out&gain=0.8").find("409 Conflict") != std::string::npos);
        assert(readFile("volumes.txt") == full);

        char effect[64];
        assert(audio->createEffect("gain", effect, sizeof(effect)) == RET_OK);
        for (const auto &entry : {
                 std::pair<const char *, const char *>{"gain", "Gain"},
                 {"distortion", "Distortion"}, {"pitch", "Pitch"},
                 {"reverb", "Reverb"}, {"gate", "Noise Gate"}, {"cut", "Cut"}}) {
            assert(audio->setEffectType(effect, audiox::effects::effectTypeFromString(entry.first)) == RET_OK);
            const auto response = request([&](int fd) { return handleAudioVolumes(&http, fd, "GET", ""); });
            assert(response.find(std::string("\"id\":\"") + effect + "\",\"name\":\"" + entry.second + "\"") != std::string::npos);
        }
        assert(audio->setEffectType(effect, audiox::effects::EFFECT_GAIN) == RET_OK);
        for (const char *invalid : {"nan", "inf", "-inf", "1junk", ""}) {
            std::string body = std::string("id=") + effect + "&enabled=0&param=gain&value=" + invalid;
            auto response = request([&](int fd) { return handleEffectSet(&http, fd, "POST", "", body.data(), body.size()); });
            assert(response.find("400 Bad Request") != std::string::npos);
            audiox::effects::SlotParams params{};
            assert(audio->getEffectParams(effect, &params) == RET_OK);
            assert(params.enabled == 1);
        }
        assert(audio->setEffectParam(effect, "gain", std::numeric_limits<float>::quiet_NaN()) == RET_ERR);
        assert(audio->setEffectParamNormalized(effect, "gain", std::numeric_limits<float>::infinity()) == RET_ERR);
        writeFile("midi_map.txt", "fx_state=fx_1,gain,1,nan,1,1,1\nfx_state=fx_2,gain,1,1,1,1,1\n");
        writeFile("fx_params.txt", "fx_param=fx_2,gain,nan\nfx_param=fx_2,gain,1junk\nfx_param=fx_2,gain,0.5\n");
        auto map = config.readMidiMapFile();
        assert(map.effectStateCount == 1 && strcmp(map.effectStates[0].effectId, "fx_2") == 0);
        assert(map.effectParamCount == 1 && map.effectParams[0].value == 0.5f);
        const auto previousMap = readFile("midi_map.txt");
        const auto previousFx = readFile("fx_params.txt");
        map.effectParams[0].value = std::numeric_limits<float>::quiet_NaN();
        assert(config.writeMidiMapFile(&map) == RET_ERR);
        map.effectParams[0].value = 0.5f;
        map.effectStates[0].gain = std::numeric_limits<float>::infinity();
        assert(config.writeMidiMapFile(&map) == RET_ERR);
        assert(readFile("midi_map.txt") == previousMap && readFile("fx_params.txt") == previousFx);

        char sources[12][64]{};
        char sinks[16][64]{};
        for (auto &id : sources) assert(audio->createEffect("gain", id, sizeof(id)) == RET_OK);
        for (auto &id : sinks) assert(audio->createEffect("gain", id, sizeof(id)) == RET_OK);
        auto helperRoutes = [&](unsigned incoming, unsigned outgoing) {
            std::string routes = "node=routing_helper\n";
            for (unsigned i = 0; i < incoming; ++i)
                routes += std::string("edge=") + sources[i] + ",routing_helper,0,0\n";
            for (unsigned i = 0; i < outgoing; ++i)
                routes += std::string("edge=routing_helper,") + sinks[i] + ",0,0\n";
            return routes;
        };
        for (const auto shape : {std::pair<unsigned, unsigned>{12, 10}, {8, 16}, {12, 12}}) {
            const auto routes = helperRoutes(shape.first, shape.second);
            writeFile("routing.txt", routes);
            const unsigned paths = shape.first * shape.second;
            const unsigned dropped = paths > AUDIO_GRAPH_MAX_EDGES ? paths - AUDIO_GRAPH_MAX_EDGES : 0;
            assert(audio->reloadRoutingGraph() == (dropped ? RET_WARN : RET_OK));
            const auto &graph = audio->routingGraph;
            assert(graph.edgeCount == paths - dropped && graph.skippedEdgeCount == dropped);
            for (unsigned i = 0; i < graph.edgeCount; ++i) {
                assert(strcmp(graph.edges[i].src, sources[i / shape.second]) == 0);
                assert(strcmp(graph.edges[i].dst, sinks[i % shape.second]) == 0);
            }
            assert(readFile("routing.txt") == routes);
            AudioGraphState compiled{};
            assert(audio->routingGraphMailbox.consume(&compiled));
            assert(memcmp(&compiled, &graph, sizeof(compiled)) == 0);
        }
        std::string mixed = helperRoutes(12, 12) +
            "edge=soundboard_out," + sinks[0] + ",1,1\nedge=missing,missing,0,0\n";
        writeFile("routing.txt", mixed);
        auto response = request([&](int fd) { return handleRoutingReload(&http, fd, "POST", ""); });
        assert(response.find("200 OK") != std::string::npos);
        assert(response.find("\"edges\":128") != std::string::npos);
        assert(response.find("\"skippedEdges\":18") != std::string::npos);
        assert(response.find("17 skipped path(s)") != std::string::npos);
        assert(strcmp(audio->routingGraph.edges[0].src, "soundboard_out") == 0);
        assert(audio->routingGraph.edges[0].srcChannel == 1);
        assert(readFile("routing.txt") == mixed);
        std::string stereo = "node=routing_helper\n";
        for (unsigned i = 0; i < 12; ++i) {
            stereo += std::string("edge=") + sources[i] + ",routing_helper,0," + std::to_string(i % 2) + "\n";
            stereo += std::string("edge=routing_helper,") + sinks[i] + "," + std::to_string(i % 2) + ",0\n";
        }
        writeFile("routing.txt", stereo);
        assert(audio->reloadRoutingGraph() == RET_OK);
        assert(audio->routingGraph.edgeCount == 72 && audio->routingGraph.skippedEdgeCount == 0);
        for (unsigned i = 0; i < 72; ++i) {
            assert(strcmp(audio->routingGraph.edges[i].src, sources[i / 6]) == 0);
            assert(strcmp(audio->routingGraph.edges[i].dst, sinks[2 * (i % 6) + (i / 6) % 2]) == 0);
        }
        writeFile("routing.txt", helperRoutes(12, 0));
        assert(audio->reloadRoutingGraph() == RET_OK);
        assert(audio->routingGraph.edgeCount == 0 && audio->routingGraph.skippedEdgeCount == 0);
    }
    for (const char *file : {"config.txt", "routing.txt", "volumes.txt", "midi_map.txt", "fx_params.txt"}) assert(unlink(file) == 0);
    assert(chdir(original) == 0);
    assert(rmdir(directory) == 0);
    puts("Graph hotplug, audio JSON, atomic volume persistence and numeric validation tests passed");
}
