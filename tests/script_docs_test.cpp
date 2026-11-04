#include "scripting/script.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    assert(!std::filesystem::exists("tests/.script-docs-test-storage"));
    unsigned blocks = 0, callbackExamples = 0;
    for (const auto &file : std::filesystem::recursive_directory_iterator("docs/lua")) {
        if (file.path().extension() != ".md") continue;
        std::ifstream input(file.path());
        assert(input);
        std::string line, source;
        bool lua = false, fence = false, callbacks = false;
        unsigned lineNumber = 0, start = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (line.rfind("```", 0) == 0) {
                if (!fence) {
                    fence = true;
                    callbacks = line == "```lua callbacks";
                    lua = line == "```lua" || callbacks;
                    start = lineNumber;
                    source.clear();
                } else {
                    fence = false;
                    if (!lua) continue;
                    char error[256]{};
                    if (!scriptValidate(source.c_str(), error, sizeof(error))) {
                        std::cerr << file.path() << ':' << start << ": " << error << '\n';
                        return 1;
                    }
                    ++blocks;
                    if (file.path().stem() == "cookbook") assert(callbacks);
                    if (callbacks) {
                        ScriptState state{};
                        ScriptPlan plan{};
                        uint16_t mask = 0;
                        if (!scriptInspectHooks(source.c_str(), mask, error, sizeof(error))) {
                            std::cerr << file.path() << ':' << start << ": " << error << '\n';
                            return 1;
                        }
                        for (unsigned i = 0; i < 3; ++i) {
                            ScriptEvent event{0, 60, 100, true, 10000 + i * 2500U, 1700000000 + i * 2};
                            event.kind = i == 1 ? ScriptEventKind::SoundFinish : ScriptEventKind::Run;
                            event.fromWeb = i != 1;
                            event.pressed = i != 1;
                            if (i == 1) std::snprintf(event.sound, sizeof(event.sound), "intro.wav");
                            if (!scriptEvaluate(source.c_str(), event, state, plan, error, sizeof(error),
                                                    nullptr, false, true, mask)) {
                                std::cerr << file.path() << ':' << start << ": " << error << '\n';
                                return 1;
                            }
                            if (source.find("hook(\"bluetooth_media_key\"") != std::string::npos) {
                                const char *keys[] = {"play", "pause", "next", "previous", "stop", "rewind",
                                    "fast_forward", "volume_up", "volume_down", "mute", "record", "eject"};
                                const char *shortcuts[] = {"MEDIA_PLAY", "MEDIA_PAUSE", "MEDIA_NEXT", "MEDIA_PREVIOUS",
                                    "MEDIA_STOP", "MEDIA_REWIND", "MEDIA_FAST_FORWARD", "MEDIA_VOLUME_UP",
                                    "MEDIA_VOLUME_DOWN", "MEDIA_MUTE", "MEDIA_RECORD", "MEDIA_EJECT"};
                                for (unsigned i = 0; i < 12; ++i) {
                                    for (bool pressed : {true, false}) {
                                        ScriptEvent event{};
                                        event.kind = ScriptEventKind::BluetoothMediaKey;
                                        event.pressed = pressed;
                                        std::snprintf(event.key, sizeof(event.key), "%s", keys[i]);
                                        std::snprintf(event.device, sizeof(event.device), "AA:BB:CC:DD:EE:FF");
                                        if (!scriptEvaluate(source.c_str(), event, state, plan, error, sizeof(error),
                                            nullptr, false, true, mask, 1)) {
                                            std::cerr << file.path() << ':' << start << ": " << error << '\n';
                                            return 1;
                                        }
                                        const bool mapped = pressed && shortcuts[i][0];
                                        assert(plan.count == (mapped ? 1U : 0U));
                                        if (mapped) {
                                            assert(plan.actions[0].kind == ScriptKind::Tap);
                                            assert(!std::strcmp(plan.actions[0].keys, shortcuts[i]));
                                        }
                                    }
                                }
                            }
                        }
                        ++callbackExamples;
                    }
                    lua = false;
                }
            } else if (lua) {
                source += line + '\n';
            }
        }
        assert(!fence);
    }
    assert(blocks > 0 && callbackExamples >= 13);
    std::cout << "Validated " << blocks << " Lua documentation examples and evaluated "
              << callbackExamples << " callback examples across Run/sound/Run events\n";
}
