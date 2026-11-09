#include "config/scenes.hpp"
#include "midi/context.hpp"
#include "audio/plugin_registry.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace audiox::scenes {
namespace {
std::mutex storeMutex;
bool fail(std::string &error, const char *message) { error = message; return false; }
bool textValid(const std::string &text, size_t max, bool empty = false) {
    if ((!empty && text.empty()) || text.size() > max) return false;
    for (unsigned char c : text) if (c < 32 || c == 127) return false;
    return true;
}
template<size_t N> void copy(char (&out)[N], const std::string &value) {
    snprintf(out, N, "%s", value.c_str());
}
bool syncDirectory(const std::string &directory) {
    int fd = open(directory.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}
bool readPath(const std::string &path, const std::string &id, Scene &out, std::string &error) {
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return fail(error, "scene not found or unreadable");
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size > static_cast<off_t>(MAX_FILE_SIZE)) {
        close(fd);
        return fail(error, "invalid scene file");
    }
    FILE *fp = fdopen(fd, "r");
    if (!fp) { close(fd); return fail(error, "cannot read scene"); }
    std::string text;
    char buffer[1024];
    while (fgets(buffer, sizeof(buffer), fp)) {
        text += buffer;
        if (text.size() > MAX_FILE_SIZE) {
            fclose(fp);
            return fail(error, "scene file capacity exceeded");
        }
    }
    bool ok = !ferror(fp);
    fclose(fp);
    if (!ok) return fail(error, "cannot read scene");
    std::istringstream input(text);
    std::string line;
    if (!std::getline(input, line) || line != "audiox-scenes 1")
        return fail(error, "unsupported scene schema");
    Scene scene;
    scene.id = id;
    bool named = false, bound = false, helped = false;
    while (std::getline(input, line)) {
        std::istringstream fields(line);
        std::string tag, value, decoded, extra;
        if (!(fields >> tag)) continue;
        if (tag == "name") {
            if (named || !(fields >> value) || !decode(value, scene.name)) return fail(error, "invalid scene name");
            named = true;
        } else if (tag == "midi") {
            if (bound || !(fields >> scene.channel >> scene.note)) return fail(error, "invalid scene MIDI binding");
            bound = true;
        } else if (tag == "helper") {
            int flag;
            if (helped || !(fields >> flag) || (flag != 0 && flag != 1)) return fail(error, "invalid scene helper");
            scene.helper = flag;
            helped = true;
        } else if (tag == "route") {
            if (!(fields >> value) || !decode(value, decoded)) return fail(error, "invalid scene route");
            scene.routes.push_back(decoded);
        } else if (tag == "volume") {
            VolumeEntry volume{};
            if (!(fields >> value >> volume.gain) || !decode(value, decoded) || !textValid(decoded, 63))
                return fail(error, "invalid scene volume");
            copy(volume.thingId, decoded);
            scene.volumes.push_back(volume);
        } else if (tag == "effect") {
            MidiEffectState effect{};
            std::string idText, type, name;
            int enabled;
            if (!(fields >> idText >> type >> enabled >> name) || !decode(idText, decoded) ||
                !textValid(decoded, 63) || type.size() > 23 || (enabled != 0 && enabled != 1))
                return fail(error, "invalid scene effect");
            copy(effect.effectId, decoded);
            copy(effect.type, type);
            if (name != "-" && (!decode(name, decoded) || !textValid(decoded, 63)))
                return fail(error, "invalid effect name");
            if (name != "-") copy(effect.name, decoded);
            effect.enabled = enabled;
            effect.gain = effect.drive = effect.clip = effect.output = 1.0f;
            scene.effects.push_back(effect);
        } else if (tag == "param") {
            MidiEffectParam param{};
            std::string name;
            if (!(fields >> value >> name >> param.value) || !decode(value, decoded) ||
                !textValid(decoded, 63) || !textValid(name, 23)) return fail(error, "invalid effect parameter");
            copy(param.effectId, decoded);
            copy(param.param, name);
            scene.params.push_back(param);
        } else return fail(error, "unknown scene field");
        if (fields >> extra) return fail(error, "extra scene field");
    }
    if (!named || !bound || !helped) return fail(error, "incomplete scene file");
    if (!validate(scene, error)) return false;
    out = std::move(scene);
    return true;
}
}

std::string encode(const std::string &text) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (unsigned char c : text) { out += hex[c >> 4]; out += hex[c & 15]; }
    return out;
}
bool decode(const std::string &text, std::string &out) {
    if (text.size() % 2 || text.size() > 512) return false;
    auto digit = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        int a = digit(text[i]), b = digit(text[i + 1]);
        if (a < 0 || b < 0 || (a == 0 && b == 0)) return false;
        out += static_cast<char>((a << 4) | b);
    }
    return true;
}
bool validate(const Scene &scene, std::string &error, bool requireAvailable) {
    if (!validId(scene.id) || !textValid(scene.name, 63)) return fail(error, "invalid scene id or name");
    if (!((scene.channel == -1 && scene.note == -1) ||
          (scene.channel >= 0 && scene.channel <= 15 && scene.note >= 0 && scene.note <= 127)))
        return fail(error, "invalid scene MIDI binding");
    if (scene.routes.size() > AUDIO_GRAPH_MAX_EDGES || scene.volumes.size() > VOLUME_ENTRIES_MAX ||
        scene.effects.size() > MIDI_EFFECT_STATES_MAX || scene.params.size() > MIDI_EFFECT_PARAMS_MAX)
        return fail(error, "scene capacity exceeded");
    for (const auto &route : scene.routes)
        if (!textValid(route, 255)) return fail(error, "invalid scene route");
    for (size_t i = 0; i < scene.params.size(); ++i) {
        const auto &p = scene.params[i];
        if (strnlen(p.effectId, sizeof(p.effectId)) == sizeof(p.effectId) ||
            strnlen(p.param, sizeof(p.param)) == sizeof(p.param) ||
            !textValid(p.effectId, 63) || !textValid(p.param, 23) || !std::isfinite(p.value))
            return fail(error, "invalid scene parameter");
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(p.effectId, scene.params[j].effectId) && !strcmp(p.param, scene.params[j].param))
                return fail(error, "duplicate scene parameter");
    }
    for (size_t i = 0; i < scene.volumes.size(); ++i) {
        const auto &v = scene.volumes[i];
        if (strnlen(v.thingId, sizeof(v.thingId)) == sizeof(v.thingId) ||
            !textValid(v.thingId, 63) || !std::isfinite(v.gain) || v.gain < 0 || v.gain > 2)
            return fail(error, "invalid scene volume");
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(v.thingId, scene.volumes[j].thingId)) return fail(error, "duplicate scene volume");
    }
    for (size_t i = 0; i < scene.effects.size(); ++i) {
        const auto &e = scene.effects[i];
        if (strnlen(e.effectId, sizeof(e.effectId)) == sizeof(e.effectId) ||
            strnlen(e.type, sizeof(e.type)) == sizeof(e.type) ||
            strnlen(e.name, sizeof(e.name)) == sizeof(e.name) ||
            !textValid(e.effectId, 63) || !textValid(e.name, 63, true) || e.enabled > 1)
            return fail(error, "invalid scene effect");
        const bool isPlugin = plugin::validTypeName(e.type);
        const uint8_t type = isPlugin ? plugin::findType(e.type) : effects::effectTypeFromString(e.type);
        if ((!isPlugin && strcmp(e.type, effects::effectTypeToString(type))) ||
            (requireAvailable && isPlugin && !plugin::available(type)))
            return fail(error, "scene requires an unavailable effect type");
        for (size_t j = 0; j < i; ++j)
            if (!strcmp(e.effectId, scene.effects[j].effectId)) return fail(error, "duplicate scene effect");
        const auto *spec = isPlugin && !plugin::metadataFor(type) ? nullptr : effects::effectTypeSpecFor(type);
        size_t parameterCount = 0;
        for (const auto &p : scene.params) if (!strcmp(p.effectId, e.effectId)) ++parameterCount;
        if (parameterCount > effects::EFFECT_PARAM_MAX) return fail(error, "effect parameter capacity exceeded");
        if (!spec) continue;
        for (uint8_t p = 0; p < spec->paramCount; ++p) {
            size_t count = 0;
            for (const auto &parameter : scene.params)
                if (!strcmp(parameter.effectId, e.effectId) && !strcmp(parameter.param, spec->params[p].name)) ++count;
            if (count == 0 && type == effects::EFFECT_PITCH &&
                !strcmp(spec->params[p].name, "quality")) continue;
            if (count != 1) return fail(error, "missing or duplicate scene parameter");
        }
    }
    for (const auto &p : scene.params) {
        const effects::EffectParamSpec *spec = nullptr;
        bool found = false, unknownPlugin = false;
        for (const auto &e : scene.effects)
            if (!strcmp(e.effectId, p.effectId)) {
                found = true;
                const bool isPlugin = plugin::validTypeName(e.type);
                const uint8_t type = isPlugin ? plugin::findType(e.type) : effects::effectTypeFromString(e.type);
                unknownPlugin = isPlugin && !plugin::metadataFor(type);
                if (!unknownPlugin) spec = effects::effectParamSpecFor(type, p.param);
            }
        if (!found || (!unknownPlugin && (!spec || p.value < spec->minValue || p.value > spec->maxValue)))
            return fail(error, "invalid scene parameter");
    }
    return true;
}
Store::Store(std::string root) : directory(std::move(root)) {}
bool Store::read(const std::string &id, Scene &out, std::string &error) const {
    std::lock_guard<std::mutex> lock(storeMutex);
    if (!validId(id)) return fail(error, "invalid scene id");
    return readPath(directory + "/" + id + ".scene", id, out, error);
}
bool Store::list(std::vector<Scene> &out, std::string &error) const {
    std::lock_guard<std::mutex> lock(storeMutex);
    out.clear();
    DIR *dir = opendir(directory.c_str());
    if (!dir) return errno == ENOENT ? true : fail(error, "cannot list scenes");
    bool ok = true;
    while (dirent *entry = readdir(dir)) {
        std::string file(entry->d_name);
        if (file.size() < 7 || file.substr(file.size() - 6) != ".scene") continue;
        std::string id = file.substr(0, file.size() - 6);
        Scene scene;
        if (!validId(id) || !readPath(directory + "/" + file, id, scene, error)) { ok = false; break; }
        out.push_back(std::move(scene));
        if (out.size() > MAX_SCENES) { ok = fail(error, "scene capacity exceeded"); break; }
    }
    closedir(dir);
    std::sort(out.begin(), out.end(), [](const Scene &a, const Scene &b) { return a.name < b.name; });
    return ok;
}
bool Store::save(const Scene &scene, std::string &error) const {
    if (!validate(scene, error)) return false;
    std::lock_guard<std::mutex> lock(storeMutex);
    if (mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) return fail(error, "cannot create scene directory");
    const std::string path = directory + "/" + scene.id + ".scene";
    DIR *dir = opendir(directory.c_str());
    if (!dir) return fail(error, "cannot list scenes");
    size_t count = 0;
    bool exists = false, conflict = false;
    while (dirent *entry = readdir(dir)) {
        std::string file(entry->d_name);
        if (file.size() < 7 || file.substr(file.size() - 6) != ".scene") continue;
        ++count;
        Scene other;
        if (!readPath(directory + "/" + file, file.substr(0, file.size() - 6), other, error)) {
            conflict = true; break;
        }
        if (other.id == scene.id) exists = true;
        else if (other.name == scene.name || (scene.note >= 0 && other.note == scene.note &&
                 other.channel == scene.channel)) {
            fail(error, "scene name or MIDI binding already in use");
            conflict = true; break;
        }
    }
    closedir(dir);
    if (conflict) return false;
    if (!exists && count >= MAX_SCENES) return fail(error, "scene capacity exceeded");
    const std::string pending = path + ".pending";
    int fd = open(pending.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (fd < 0) return fail(error, "cannot write scene");
    FILE *fp = fdopen(fd, "w");
    if (!fp) { close(fd); unlink(pending.c_str()); return fail(error, "cannot write scene"); }
    fprintf(fp, "audiox-scenes 1\nname %s\nmidi %d %d\nhelper %d\n",
        encode(scene.name).c_str(), scene.channel, scene.note, scene.helper ? 1 : 0);
    for (const auto &route : scene.routes) fprintf(fp, "route %s\n", encode(route).c_str());
    for (const auto &v : scene.volumes) fprintf(fp, "volume %s %.9g\n", encode(v.thingId).c_str(), double(v.gain));
    for (const auto &e : scene.effects)
        fprintf(fp, "effect %s %s %u %s\n", encode(e.effectId).c_str(), e.type, e.enabled,
            e.name[0] ? encode(e.name).c_str() : "-");
    for (const auto &p : scene.params)
        fprintf(fp, "param %s %s %.9g\n", encode(p.effectId).c_str(), p.param, double(p.value));
    bool ok = fflush(fp) == 0 && fsync(fd) == 0;
    if (fclose(fp) != 0) ok = false;
    if (ok) ok = rename(pending.c_str(), path.c_str()) == 0;
    if (ok) ok = syncDirectory(directory);
    if (!ok) { unlink(pending.c_str()); return fail(error, "cannot durably save scene"); }
    return true;
}
bool Store::remove(const std::string &id, std::string &error) const {
    std::lock_guard<std::mutex> lock(storeMutex);
    if (!validId(id)) return fail(error, "invalid scene id");
    if (unlink((directory + "/" + id + ".scene").c_str()) != 0) return fail(error, "scene not found or cannot delete");
    return syncDirectory(directory) ? true : fail(error, "cannot durably delete scene");
}

bool capture(Audiox *app, Scene &scene, std::string &error) {
    if (!app || !app->audio || !app->config) return fail(error, "audio unavailable");
    std::lock_guard<std::mutex> lock(app->audio->sceneMutex);
    auto &audio = *app->audio;
    auto router = app->config->router();
    int count = router.getRouteCount();
    if (count < 0) return fail(error, "cannot read routing");
    scene.routes.clear();
    scene.helper = router.hasRoutingHelper();
    for (int i = 0; i < count; ++i) {
        char route[256]{};
        router.getRoute(i, route, sizeof(route));
        scene.routes.emplace_back(route);
    }
    scene.volumes.clear();
    {
        std::lock_guard<std::mutex> gainLock(audio.gainsMutex);
        for (const auto &gain : audio.thingGains) {
            VolumeEntry entry{};
            copy(entry.thingId, gain.first);
            entry.gain = gain.second;
            scene.volumes.push_back(entry);
        }
    }
    const MidiMapData map = app->config->readMidiMapFile();
    scene.effects.clear();
    scene.params.clear();
    {
        std::lock_guard<std::mutex> effectLock(audio.effectsMutex);
        for (const auto &entry : audio.effectStates) {
            const auto &params = entry.second;
            MidiEffectState state{};
            copy(state.effectId, entry.first);
            copy(state.type, effects::effectTypeToString(params.type));
            state.enabled = params.enabled;
            state.gain = state.drive = state.clip = state.output = 1.0f;
            for (uint32_t i = 0; i < map.effectStateCount && i < MIDI_EFFECT_STATES_MAX; ++i)
                if (entry.first == map.effectStates[i].effectId) copy(state.name, map.effectStates[i].name);
            scene.effects.push_back(state);
            const auto *spec = effects::effectTypeSpecFor(params.type);
            if (!spec) return fail(error, "unavailable effect type");
            for (uint8_t i = 0; i < spec->paramCount; ++i) {
                MidiEffectParam param{};
                copy(param.effectId, entry.first);
                copy(param.param, spec->params[i].name);
                if (effects::getSlotParamValue(params, param.param, &param.value) != RET_OK)
                    return fail(error, "cannot capture parameter");
                scene.params.push_back(param);
            }
        }
    }
    return validate(scene, error, true);
}

bool recall(Audiox *app, const Scene &scene, std::string &error) {
    if (!app || !app->audio || !app->config) return fail(error, "audio unavailable");
    if (!validate(scene, error, true)) return false;
    auto &audio = *app->audio;
    std::lock_guard<std::mutex> lock(audio.sceneMutex);
    if (audio.sceneReleaseGeneration.load(std::memory_order_acquire))
        return fail(error, "previous scene transition is still pending");
    std::unordered_map<std::string, effects::SlotParams> states;
    std::unordered_map<std::string, std::unique_ptr<plugin::PluginHost>> hosts;
    uint32_t nextId = 1;
    for (const auto &effect : scene.effects) {
        effects::SlotParams params{};
        params.type = effects::effectTypeFromString(effect.type);
        params.enabled = effect.enabled;
        effects::setSlotDefaultsForType(&params, params.type);
        for (const auto &parameter : scene.params)
            if (!strcmp(parameter.effectId, effect.effectId))
                (void)effects::setSlotParamValue(&params, parameter.param, parameter.value);
        if (plugin::isPluginType(params.type)) {
            auto host = audio.preparePlugin(effect.effectId, params.type);
            if (!host) return fail(error, "scene plugin could not start; setup unchanged");
            params.pluginHost = host.get();
            hosts[effect.effectId] = std::move(host);
        }
        states[effect.effectId] = params;
        unsigned id = 0;
        if (sscanf(effect.effectId, "fx_%u", &id) == 1 && id < UINT32_MAX) nextId = std::max(nextId, id + 1);
    }
    MidiMapData previous = app->config->readMidiMapFile();
    MidiMapData map = previous;
    map.effectStateCount = scene.effects.size();
    map.effectParamCount = scene.params.size();
    std::copy(scene.effects.begin(), scene.effects.end(), map.effectStates);
    std::copy(scene.params.begin(), scene.params.end(), map.effectParams);
    // Fade all sinks before inventory/topology changes, then release after the
    // audio thread has consumed the new graph and its gain snapshot.
    audio.sceneSilent.store(false, std::memory_order_release);
    audio.sceneReleaseGeneration.store(0, std::memory_order_release);
    audio.sceneMuted.store(true, std::memory_order_release);
    const bool running = audio.processingThreadStarted &&
        audio.processingThreadRun.load(std::memory_order_acquire);
    bool silent = !running;
    for (unsigned i = 0; running && i < 250; ++i) {
        if (audio.sceneSilent.load(std::memory_order_acquire)) { silent = true; break; }
        usleep(1000);
    }
    if (!silent) {
        audio.sceneMuted.store(false, std::memory_order_release);
        return fail(error, "audio transition timed out; scene unchanged");
    }
    auto router = app->config->router();
    std::vector<std::string> oldRoutes;
    const bool oldHelper = router.hasRoutingHelper();
    int oldCount = router.getRouteCount();
    if (oldCount < 0) { audio.sceneMuted.store(false); return fail(error, "cannot read routing"); }
    for (int i = 0; i < oldCount; ++i) {
        char route[256]{};
        router.getRoute(i, route, sizeof(route));
        oldRoutes.emplace_back(route);
    }
    VolumeEntry oldVolumes[VOLUME_ENTRIES_MAX]{};
    uint32_t oldVolumeCount = 0;
    if (app->config->readVolumesFile(oldVolumes, &oldVolumeCount, VOLUME_ENTRIES_MAX) == RET_ERR) {
        audio.sceneMuted.store(false, std::memory_order_release);
        return fail(error, "cannot read current volume configuration");
    }
    bool ok = app->config->writeMidiMapFile(&map) == RET_OK;
    if (ok) ok = app->config->writeVolumesFile(scene.volumes.data(), scene.volumes.size()) == RET_OK;
    if (ok) ok = router.replaceSceneRoutes(scene.routes, scene.helper) == RET_OK;
    if (!ok) {
        (void)app->config->writeMidiMapFile(&previous);
        (void)app->config->writeVolumesFile(oldVolumes, oldVolumeCount);
        (void)router.replaceSceneRoutes(oldRoutes, oldHelper);
        audio.sceneMuted.store(false, std::memory_order_release);
        return fail(error, "cannot persist recalled setup; previous setup restored");
    }
    {
        std::lock_guard<std::mutex> effectLock(audio.effectsMutex);
        while (!audio.pluginHosts.empty()) audio.retirePlugin(audio.pluginHosts.begin()->first.c_str());
        audio.effectStates.swap(states);
        audio.pluginHosts.swap(hosts);
        audio.pluginFaultReported.clear();
        audio.nextEffectId = nextId;
        audio.effectStatesSeq.fetch_add(1, std::memory_order_release);
    }
    {
        std::lock_guard<std::mutex> gainLock(audio.gainsMutex);
        audio.thingGains.clear();
        for (const auto &volume : scene.volumes) audio.thingGains[volume.thingId] = volume.gain;
    }
    if (app->midi) app->midi->cachedMidiMap = map;
    const int graphResult = audio.reloadRoutingGraph();
    // The mute release is deferred to graph consumption in the audio thread.
    if (!running || graphResult == RET_ERR) audio.sceneMuted.store(false, std::memory_order_release);
    else {
        std::lock_guard<std::mutex> graphLock(audio.routingGraphMutex);
        audio.sceneReleaseGeneration.store(audio.routingGraphPublished.generation, std::memory_order_release);
    }
    for (unsigned i = 0; running && i < 250; ++i) {
        if (!audio.sceneReleaseGeneration.load(std::memory_order_acquire)) break;
        usleep(1000);
    }
    if (graphResult == RET_ERR) return fail(error, "scene saved but routing reload failed");
    audio.activeSceneId = scene.id;
    audio.sceneRecallSeq.fetch_add(1, std::memory_order_release);
    if (app->midi) app->midi->refreshLightingFromState();
    return true;
}
bool recallMidi(Audiox *app, uint8_t channel, uint8_t note) {
    static std::mutex cacheMutex;
    static std::vector<Scene> scenes;
    static ConfigFileStamp stamp{};
    static bool loaded = false;
    std::lock_guard<std::mutex> lock(cacheMutex);
    ConfigFileStamp current{};
    std::string error;
    if (!configFileStamp(CONFIG_ROOT_DIR "/scenes", &current)) return false;
    if (!loaded || !configFileStampEqual(stamp, current)) {
        if (!Store().list(scenes, error)) return false;
        stamp = current;
        loaded = true;
    }
    for (const auto &scene : scenes) {
        if (scene.channel != channel || scene.note != note) continue;
        if (!recall(app, scene, error)) printf("[SCENES] MIDI recall failed: %s\n", error.c_str());
        return true;
    }
    return false;
}
}
