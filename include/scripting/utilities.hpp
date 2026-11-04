#pragma once

#include "scripting/script.hpp"

struct lua_State;

void scriptUtilityCheck(lua_State *lua, bool write, bool read);
ConfigStore *scriptUtilityConfig(lua_State *lua);
ScriptAction *scriptUtilityAppend(lua_State *lua, ScriptKind kind);
void scriptRegisterUtilities(lua_State *lua);
