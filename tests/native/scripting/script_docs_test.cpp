#include "scripting/script.hpp"
#include "scripting/storage.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <utility>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
int luaopen_base(lua_State *lua);
}

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
                        uint32_t mask = 0;
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
                                                    nullptr, false, true, mask, 1)) {
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
    {
        std::ifstream input("docs/public/scripts/bluetooth-default-sink.lua");
        assert(input);
        const std::string source((std::istreambuf_iterator<char>(input)), {});
        char error[256]{};
        uint32_t mask = 0;
        ScriptUi controls{};
        assert(scriptValidate(source.c_str(), error, sizeof(error)));
        assert(scriptInspectHooks(source.c_str(), mask, error, sizeof(error)));
        assert(scriptInspectUi(source.c_str(), controls, error, sizeof(error)));
        ScriptState state{};
        ScriptPlan plan{};
        ScriptEvent event{};
        event.kind = ScriptEventKind::Run;
        if (!scriptEvaluate(source.c_str(), event, state, plan, error, sizeof(error),
                            nullptr, true, true, mask, 1)) {
            std::cerr << "Bluetooth default-sink script: " << error << '\n';
            return 1;
        }
        for (unsigned i = 0; i < plan.count; ++i)
            assert(plan.actions[i].kind != ScriptKind::DesktopDefaultSet);
        auto configured = source;
        const auto replace = [](std::string &text, const char *from, const char *to) {
            const auto position = text.find(from);
            assert(position != std::string::npos);
            text.replace(position, std::strlen(from), to);
        };
        replace(configured, "type = \"bt-device\", default = \"\"",
                "type = \"bt-device\", default = \"AA:BB:CC:DD:EE:FF\"");
        replace(configured, "type = \"host-device\", default = \"\"",
                "type = \"host-device\", default = \"sink.audiox\"");
        auto connected = configured;
        auto disconnected = configured;
        replace(connected, "bluetooth.devices()",
                "{ready=true, devices={{address='AA:BB:CC:DD:EE:FF', connected=true}}}");
        replace(disconnected, "bluetooth.devices()",
                "{ready=true, devices={{address='AA:BB:CC:DD:EE:FF', connected=false}}}");
        const auto evaluate = [&](const std::string &script, ScriptEventKind kind,
                                  const char *name, bool pending) {
            event.kind = kind;
            event.monotonicMs += 1000;
            if (!scriptEvaluate(script.c_str(), event, state, plan, error, sizeof(error),
                                nullptr, true, true, mask, 1, nullptr,
                                DesktopMedia::Unavailable, name, 1, pending)) {
                std::cerr << "Bluetooth default-sink evaluation: " << error << '\n';
                assert(false);
            }
        };
        const auto requested = [&]() -> const char * {
            for (unsigned i = 0; i < plan.count; ++i)
                if (plan.actions[i].kind == ScriptKind::DesktopDefaultSet) return plan.actions[i].text;
            return nullptr;
        };
        evaluate(connected, ScriptEventKind::BluetoothConnect, "sink.original", false);
        assert(requested() && !std::strcmp(requested(), "sink.audiox"));
        evaluate(disconnected, ScriptEventKind::BluetoothDisconnect, "sink.original", true);
        assert(!requested());
        evaluate(disconnected, ScriptEventKind::Timer, "sink.audiox", false);
        assert(requested() && !std::strcmp(requested(), "sink.original"));
        evaluate(disconnected, ScriptEventKind::Timer, "sink.original", false);
        assert(!requested());
        lua_State *lua = luaL_newstate();
        assert(lua);
        luaL_requiref(lua, "_G", luaopen_base, 1);
        lua_pop(lua, 1);
        if (luaL_dofile(lua, "tests/native/scripting/bluetooth_default_test.lua") != LUA_OK) {
            std::cerr << "Bluetooth default-sink scenarios: " << lua_tostring(lua, -1) << '\n';
            lua_close(lua);
            return 1;
        }
        lua_close(lua);
    }
    {
        std::ifstream input("docs/public/scripts/desktop-default-toggle.lua");
        assert(input);
        std::string source((std::istreambuf_iterator<char>(input)), {});
        char error[256]{};
        uint32_t mask = 0;
        ScriptUi controls{};
        assert(scriptValidate(source.c_str(), error, sizeof(error)));
        assert(scriptInspectHooks(source.c_str(), mask, error, sizeof(error)));
        assert(scriptInspectUi(source.c_str(), controls, error, sizeof(error)));
        const auto position = source.find("type = \"host-device\", default = \"\"");
        assert(position != std::string::npos);
        source.replace(position, std::strlen("type = \"host-device\", default = \"\""),
                       "type = \"host-device\", default = \"sink.audiox\"");
        ScriptState state{};
        ScriptPlan plan{};
        ScriptEvent event{};
        event.kind = ScriptEventKind::Ui;
        std::snprintf(event.key, sizeof(event.key), "toggle");
        const auto evaluate = [&](const char *name, uint32_t session, bool pending) -> const char * {
            if (!scriptEvaluate(source.c_str(), event, state, plan, error, sizeof(error),
                                nullptr, true, true, mask, 1, nullptr,
                                DesktopMedia::Unavailable, name, session, pending)) {
                std::cerr << "Desktop toggle script: " << error << '\n';
                assert(false);
            }
            for (unsigned i = 0; i < plan.count; ++i)
                if (plan.actions[i].kind == ScriptKind::DesktopDefaultSet) return plan.actions[i].text;
            return static_cast<const char *>(nullptr);
        };
        assert(!evaluate(nullptr, 0, false));
        assert(!evaluate("sink.original", 1, true));
        const char *requested = evaluate("sink.original", 1, false);
        assert(requested && !std::strcmp(requested, "sink.audiox"));
        assert(!evaluate("sink.original", 1, true));
        requested = evaluate("sink.audiox", 1, false);
        assert(requested && !std::strcmp(requested, "sink.original"));
        requested = evaluate("sink.audiox", 1, false);
        assert(requested && !std::strcmp(requested, "sink.original"));
        requested = evaluate("sink.headphones", 1, false);
        assert(requested && !std::strcmp(requested, "sink.audiox"));
        requested = evaluate("sink.audiox", 1, false);
        assert(requested && !std::strcmp(requested, "sink.headphones"));
        assert(!evaluate("sink.audiox", 2, false));
    }
    {
        std::ifstream input("docs/public/scripts/output-toggle.lua");
        assert(input);
        const std::string source((std::istreambuf_iterator<char>(input)), {});
        char error[256]{};
        uint32_t mask = 0;
        ScriptUi controls{};
        assert(scriptValidate(source.c_str(), error, sizeof(error)));
        assert(scriptInspectHooks(source.c_str(), mask, error, sizeof(error)));
        assert(scriptInspectUi(source.c_str(), controls, error, sizeof(error)));
        ScriptState state{};
        ScriptPlan plan{};
        const auto press = [&](ScriptEventKind kind, uint8_t velocity, bool pressed) {
            ScriptEvent event{};
            event.kind = kind;
            event.velocity = velocity;
            event.pressed = pressed;
            if (!scriptEvaluate(source.c_str(), event, state, plan, error, sizeof(error), nullptr, false, true, mask, 1)) {
                std::cerr << "Output toggle script: " << error << '\n';
                assert(false);
            }
            int headphones = -1, speakers = -1;
            for (unsigned i = 0; i < plan.count; ++i) {
                if (plan.actions[i].kind != ScriptKind::EffectEnable) continue;
                if (!std::strcmp(plan.actions[i].target, "fx_1")) headphones = int(plan.actions[i].value);
                if (!std::strcmp(plan.actions[i].target, "fx_2")) speakers = int(plan.actions[i].value);
            }
            return std::pair{headphones, speakers};
        };
        assert((press(ScriptEventKind::Boot, 0, true) == std::pair{0, 1}));
        assert((press(ScriptEventKind::NoteOn, 100, true) == std::pair{1, 0}));
        assert((press(ScriptEventKind::NoteOff, 0, false) == std::pair{-1, -1}));
        assert((press(ScriptEventKind::ControlChange, 0, false) == std::pair{-1, -1}));
        assert((press(ScriptEventKind::ControlChange, 127, false) == std::pair{0, 1}));
        assert((press(ScriptEventKind::Boot, 0, true) == std::pair{0, 1}));
    }
    {
        std::ifstream input("docs/public/scripts/effect-group-toggle.lua");
        assert(input);
        const std::string source((std::istreambuf_iterator<char>(input)), {});
        char error[256]{};
        uint32_t mask = 0;
        ScriptUi controls{};
        assert(scriptValidate(source.c_str(), error, sizeof(error)));
        assert(scriptInspectHooks(source.c_str(), mask, error, sizeof(error)));
        assert(scriptInspectUi(source.c_str(), controls, error, sizeof(error)));
        ScriptStorage storage(2);
        ScriptState state{};
        ScriptPlan plan{};
        // Returns the shared enabled state (-1 if untouched) and the pad velocity (-1 if not sent).
        const auto send = [&](ScriptEventKind kind, bool pressed) {
            ScriptEvent event{};
            event.kind = kind;
            event.pressed = pressed;
            event.velocity = pressed ? 100 : 0;
            if (!scriptEvaluate(source.c_str(), event, state, plan, error, sizeof(error), nullptr, false, true, mask, 2)) {
                std::cerr << "Effect group toggle script: " << error << '\n';
                assert(false);
            }
            int enabled = -1, light = -1;
            unsigned effectCount = 0;
            for (unsigned i = 0; i < plan.count; ++i) {
                const auto &action = plan.actions[i];
                if (action.kind == ScriptKind::EffectEnable) {
                    assert(enabled == -1 || enabled == int(action.value));
                    enabled = int(action.value);
                    ++effectCount;
                } else if (action.kind == ScriptKind::Midi) {
                    assert(action.midiSize == 3 && action.midiBytes[0] == 0x90 && action.midiBytes[1] == 71);
                    light = action.midiBytes[2];
                } else if (action.kind == ScriptKind::ScriptConfigSet) {
                    assert(storage.setSetting(action.setting) == RET_OK);
                }
            }
            assert(effectCount == 0 || effectCount == 4);
            return std::pair{enabled, light};
        };
        assert((send(ScriptEventKind::DeviceConnect, true) == std::pair{1, 1}));
        assert((send(ScriptEventKind::NoteOn, true) == std::pair{0, 0}));
        assert((send(ScriptEventKind::NoteOff, false) == std::pair{-1, -1}));
        assert((send(ScriptEventKind::DeviceConnect, true) == std::pair{0, 0}));
        assert((send(ScriptEventKind::Run, true) == std::pair{1, 1}));
        std::filesystem::remove_all("tests/.script-docs-test-storage");
    }
    std::cout << "Validated " << blocks << " Lua documentation examples and evaluated "
              << callbackExamples << " callback examples across Run/sound/Run events\n";
}
