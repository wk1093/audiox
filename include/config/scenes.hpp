#pragma once

#include "audio/context.hpp"
#include "config/scene_schema.hpp"
#include <string>
#include <vector>

namespace audiox::scenes {
struct Scene {
    std::string id;
    std::string name;
    int channel = -1;
    int note = -1;
    bool helper = false;
    std::vector<std::string> routes;
    std::vector<VolumeEntry> volumes;
    std::vector<MidiEffectState> effects;
    std::vector<MidiEffectParam> params;
};
bool validate(const Scene &scene, std::string &error, bool requireAvailable = false);
std::string encode(const std::string &text);
bool decode(const std::string &text, std::string &out);
class Store {
    std::string directory;
public:
    explicit Store(std::string root = CONFIG_ROOT_DIR "/scenes");
    bool list(std::vector<Scene> &out, std::string &error) const;
    bool read(const std::string &id, Scene &out, std::string &error) const;
    bool save(const Scene &scene, std::string &error) const;
    bool remove(const std::string &id, std::string &error) const;
};
bool capture(Audiox *app, Scene &scene, std::string &error);
bool recall(Audiox *app, const Scene &scene, std::string &error);
bool recallMidi(Audiox *app, uint8_t channel, uint8_t note);
}
