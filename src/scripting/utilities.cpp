#include "scripting/utilities.hpp"
#include "scripting/device.hpp"
#include "config/context.hpp"
#include "http/logs.hpp"

#include <cmath>
#include <cstring>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
}

namespace {

void arity(lua_State *lua, int min, int max) {
    if (lua_gettop(lua) < min || lua_gettop(lua) > max)
        luaL_error(lua, "invalid utility argument count");
}

const char *text(lua_State *lua, int index, size_t max) {
    size_t length = 0;
    if (lua_type(lua, index) != LUA_TSTRING)
        luaL_error(lua, "expected string argument");
    const char *value = lua_tolstring(lua, index, &length);
    if (!length || length > max || memchr(value, '\0', length))
        luaL_error(lua, "invalid utility string length or NUL");
    return value;
}

ScriptStateEntry scalar(lua_State *lua, int index) {
    ScriptStateEntry value{};
    if (lua_type(lua, index) == LUA_TBOOLEAN) {
        value.kind = ScriptValueKind::Boolean;
        value.booleanValue = lua_toboolean(lua, index);
    } else if (lua_type(lua, index) == LUA_TNUMBER) {
        value.kind = ScriptValueKind::Number;
        value.numberValue = lua_tonumber(lua, index);
        if (!std::isfinite(value.numberValue)) luaL_error(lua, "expected finite utility number");
        value.numberIsInteger = lua_isinteger(lua, index);
        if (value.numberIsInteger) value.integerValue = lua_tointeger(lua, index);
    } else if (lua_type(lua, index) == LUA_TSTRING) {
        value.kind = ScriptValueKind::String;
        size_t length = 0;
        const char *source = lua_tolstring(lua, index, &length);
        if (length > SCRIPT_STATE_STRING_MAX || memchr(source, '\0', length))
            luaL_error(lua, "utility string exceeds 127 bytes or contains NUL");
        memcpy(value.stringValue, source, length);
    } else luaL_error(lua, "expected boolean, number or string utility value");
    return value;
}

void boolean(lua_State *lua, const char *key, bool value) {
    lua_pushboolean(lua, value);
    lua_setfield(lua, -2, key);
}

void string(lua_State *lua, const char *key, const char *value) {
    lua_pushstring(lua, value);
    lua_setfield(lua, -2, key);
}

void number(lua_State *lua, const char *key, uint32_t value) {
    lua_pushinteger(lua, value);
    lua_setfield(lua, -2, key);
}

int settingsGet(lua_State *lua) {
    arity(lua, 0, 0);
    scriptUtilityCheck(lua, false, true);
    auto *config = scriptUtilityConfig(lua);
    ConfigData data{};
    if (!config || config->readScriptSettings(&data) != RET_OK)
        return luaL_error(lua, "settings.get: active configuration unavailable");
    lua_createtable(lua, 0, 8);
    number(lua, "usb_sample_rate", data.sampleRate);
    number(lua, "usb_playback_channels", data.playbackChannels);
    number(lua, "usb_capture_channels", data.captureChannels);
    number(lua, "usb_sample_size", data.sampleSize);
    boolean(lua, "keyboard_macros", data.keyboardMacros != 0);
    string(lua, "soundboard_mode", soundboardModeToString(data.soundboardMode));
    string(lua, "framebuffer_view", data.framebufferSoundboard ? "soundboard" : "meters");
    string(lua, "midi_passthrough", data.midiPassthrough == 1 ? "unmapped" : data.midiPassthrough == 2 ? "all" : "off");
    return 1;
}

int settingsSet(lua_State *lua) {
    arity(lua, 2, 2);
    scriptUtilityCheck(lua, true, false);
    const char *key = text(lua, 1, 31);
    const auto value = scalar(lua, 2);
    ConfigData trial{};
    if (!applyScriptSetting(trial, key, value))
        return luaL_error(lua, "settings.set: unknown setting, invalid type or range");
    auto *action = scriptUtilityAppend(lua, ScriptKind::SettingsSet);
    snprintf(action->target, sizeof(action->target), "%s", key);
    action->setting = value;
    return 0;
}

int restart(lua_State *lua) {
    arity(lua, 0, 0);
    scriptUtilityCheck(lua, true, false);
    scriptUtilityAppend(lua, ScriptKind::SystemRestart);
    return 0;
}

int logs(lua_State *lua) {
    arity(lua, 0, 2);
    scriptUtilityCheck(lua, false, true);
    const char *source = lua_gettop(lua) ? text(lua, 1, 6) : "stdout";
    lua_Integer limit = 32;
    if (lua_gettop(lua) == 2) {
        if (!lua_isinteger(lua, 2)) return luaL_error(lua, "system.logs: limit must be an integer");
        limit = lua_tointeger(lua, 2);
    }
    if ((strcmp(source, "stdout") && strcmp(source, "stderr")) || limit < 1 || limit > 128)
        return luaL_error(lua, "system.logs: expected stdout/stderr and 1..128 lines");
    char output[4097]{};
    size_t length = 0;
    int result = readSystemLogText(source, static_cast<unsigned>(limit), output, sizeof(output), &length);
    if (result != 0) return luaL_error(lua, "system.logs: %s", strerror(-result));
    lua_pushlstring(lua, output, length);
    return 1;
}

int bluetoothDevices(lua_State *lua) {
    arity(lua, 0, 0);
    scriptUtilityCheck(lua, false, true);
    auto *config = scriptUtilityConfig(lua);
    ScriptBluetoothSnapshot status{};
    if (!scriptBluetoothSnapshot(config ? config->app : nullptr, status))
        return luaL_error(lua, "bluetooth.devices: Bluetooth subsystem unavailable");
    lua_createtable(lua, 0, 5);
    boolean(lua, "ready", status.ready);
    boolean(lua, "scanning", status.scanning);
    boolean(lua, "pairing", status.pairing);
    string(lua, "message", status.message);
    lua_createtable(lua, static_cast<int>(status.count), 0);
    for (unsigned i = 0; i < status.count; ++i) {
        const auto &device = status.devices[i];
        lua_createtable(lua, 0, 9);
        string(lua, "address", device.address);
        string(lua, "name", device.name);
        boolean(lua, "paired", device.paired);
        boolean(lua, "auto_connect", device.autoConnect);
        boolean(lua, "connected", device.connected);
        boolean(lua, "connecting", device.connecting);
        boolean(lua, "audio_source", device.audioSource);
        boolean(lua, "audio_sink", device.audioSink);
        lua_rawseti(lua, -2, i + 1);
    }
    lua_setfield(lua, -2, "devices");
    return 1;
}

int bluetoothAction(lua_State *lua) {
    const auto kind = static_cast<ScriptKind>(lua_tointeger(lua, lua_upvalueindex(1)));
    const bool scan = kind == ScriptKind::BluetoothScan;
    const bool disconnect = kind == ScriptKind::BluetoothDisconnect;
    const bool automatic = kind == ScriptKind::BluetoothAutoConnect;
    arity(lua, scan || disconnect ? 0 : automatic ? 2 : 1, scan ? 0 : automatic ? 2 : 1);
    scriptUtilityCheck(lua, true, false);
    const char *address = "";
    if (!scan && lua_gettop(lua)) {
        address = text(lua, 1, BT_ADDRESS_TEXT - 1);
        bt::Address parsed{};
        if (!bt::parseAddress(address, &parsed) || bt::isZeroAddress(parsed))
            return luaL_error(lua, "Bluetooth action: invalid MAC address");
    }
    if (automatic && lua_type(lua, 2) != LUA_TBOOLEAN)
        return luaL_error(lua, "bluetooth.auto_connect: expected boolean");
    auto *action = scriptUtilityAppend(lua, kind);
    snprintf(action->target, sizeof(action->target), "%s", address);
    if (automatic) action->value = lua_toboolean(lua, 2);
    return 0;
}

void function(lua_State *lua, const char *name, lua_CFunction value) {
    lua_pushcfunction(lua, value);
    lua_setfield(lua, -2, name);
}

} // namespace

void scriptRegisterUtilities(lua_State *lua) {
    lua_newtable(lua);
    function(lua, "get", settingsGet);
    function(lua, "set", settingsSet);
    lua_setglobal(lua, "settings");
    lua_newtable(lua);
    function(lua, "restart", restart);
    function(lua, "logs", logs);
    lua_setglobal(lua, "system");
    lua_newtable(lua);
    function(lua, "devices", bluetoothDevices);
    const char *names[] = {"scan", "pair", "connect", "disconnect", "unpair", "auto_connect"};
    const ScriptKind kinds[] = {ScriptKind::BluetoothScan, ScriptKind::BluetoothPair,
        ScriptKind::BluetoothConnect, ScriptKind::BluetoothDisconnect,
        ScriptKind::BluetoothUnpair, ScriptKind::BluetoothAutoConnect};
    for (unsigned i = 0; i < 6; ++i) {
        lua_pushinteger(lua, static_cast<int>(kinds[i]));
        lua_pushcclosure(lua, bluetoothAction, 1);
        lua_setfield(lua, -2, names[i]);
    }
    lua_setglobal(lua, "bluetooth");
}
