#include "midi/script.hpp"

#include <cassert>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    unsigned blocks = 0, recipes = 0;
    for (const auto &file : std::filesystem::recursive_directory_iterator("docs/lua")) {
        if (file.path().extension() != ".md") continue;
        std::ifstream input(file.path());
        assert(input);
        std::string line, source;
        bool lua = false, fence = false;
        unsigned lineNumber = 0, start = 0;
        while (std::getline(input, line)) {
            ++lineNumber;
            if (line.rfind("```", 0) == 0) {
                if (!fence) {
                    fence = true;
                    lua = line == "```lua";
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
                    if (file.path().stem() == "cookbook") {
                        MidiScriptState state{};
                        MidiScriptPlan plan{};
                        for (unsigned i = 0; i < 2; ++i) {
                            MidiScriptEvent event{0, 60, 100, true, 10000 + i * 2500U, 1700000000 + i * 2};
                            if (!midiEvaluateScript(source.c_str(), event, state, plan, error, sizeof(error))) {
                                std::cerr << file.path() << ':' << start << ": " << error << '\n';
                                return 1;
                            }
                        }
                        ++recipes;
                    }
                    lua = false;
                }
            } else if (lua) {
                source += line + '\n';
            }
        }
        assert(!fence);
    }
    assert(blocks > 0 && recipes > 0);
    std::cout << "Validated " << blocks << " Lua documentation examples and evaluated "
              << recipes << " cookbook scripts twice each\n";
}
