#include "midi/script.hpp"

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
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
                    if (!midiValidateScript(source.c_str(), error, sizeof(error))) {
                        std::cerr << file.path() << ':' << start << ": " << error << '\n';
                        return 1;
                    }
                    ++blocks;
                    if (file.path().stem() == "cookbook") assert(callbacks);
                    if (callbacks) {
                        MidiScriptState state{};
                        MidiScriptPlan plan{};
                        uint16_t mask = 0;
                        if (!midiInspectHooks(source.c_str(), mask, error, sizeof(error))) {
                            std::cerr << file.path() << ':' << start << ": " << error << '\n';
                            return 1;
                        }
                        for (unsigned i = 0; i < 3; ++i) {
                            MidiScriptEvent event{0, 60, 100, true, 10000 + i * 2500U, 1700000000 + i * 2};
                            event.kind = i == 1 ? MidiScriptEventKind::SoundFinish : MidiScriptEventKind::Run;
                            event.fromWeb = i != 1;
                            event.pressed = i != 1;
                            if (i == 1) std::snprintf(event.sound, sizeof(event.sound), "intro.wav");
                            if (!midiEvaluateScript(source.c_str(), event, state, plan, error, sizeof(error),
                                                    nullptr, false, true, mask)) {
                                std::cerr << file.path() << ':' << start << ": " << error << '\n';
                                return 1;
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
