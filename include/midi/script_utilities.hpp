#pragma once

#include "midi/script.hpp"

struct lua_State;

void midiScriptUtilityCheck(lua_State *lua, bool write, bool read);
ConfigStore *midiScriptUtilityConfig(lua_State *lua);
MidiScriptAction *midiScriptUtilityAppend(lua_State *lua, MidiScriptKind kind);
void midiScriptRegisterUtilities(lua_State *lua);
