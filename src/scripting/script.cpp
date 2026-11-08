#include "scripting/script.hpp"
#include "scripting/storage.hpp"
#include "scripting/utilities.hpp"
#include "config/context.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <algorithm>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
int luaopen_base(lua_State *lua);
int luaopen_string(lua_State *lua);
int luaopen_table(lua_State *lua);
}

namespace {
constexpr uint64_t DURATION_MAX = 60000;
char hooksRegistryKey;

struct Compiler {
    const ScriptMeters *meters;
    unsigned meterReads;
    ScriptPlan *plan;
    ScriptState *state;
    const ScriptState *initialState;
    const ScriptEvent *event;
    const char *source;
    size_t length;
    size_t memory;
    uint64_t duration;
    unsigned typeChars;
    unsigned instructions;
    bool syntaxOnly;
    uint64_t randomState;
    unsigned helperWork;
    ConfigStore *config;
    unsigned configReads;
    bool configWriteAllowed;
    bool hookMode;
    bool registering;
    unsigned hookCount;
    uint16_t hookMask;
    uint16_t expectedHookMask;
    uint16_t *discoveredMask;
    ScriptActivity activity;
    ScriptActivity *discoveredActivity;
    uint32_t scriptId;
    ScriptUi ui;
    ScriptUi *inspectedUi;
    ScriptState settings;
    bool settingsLoaded;
    unsigned storageReads;
    std::string storageText;
    std::vector<std::string> storageNames;
};

void *allocate(void *user, void *pointer, size_t oldSize, size_t newSize) {
    auto *compiler = static_cast<Compiler *>(user);
    if (!pointer) oldSize = 0;
    if (!newSize) {
        std::free(pointer);
        compiler->memory -= oldSize;
        return nullptr;
    }
    if (newSize > SCRIPT_MEMORY_MAX - (compiler->memory - oldSize)) return nullptr;
    void *result = std::realloc(pointer, newSize);
    if (result) compiler->memory = compiler->memory - oldSize + newSize;
    return result;
}

Compiler *context(lua_State *lua) {
    return *static_cast<Compiler **>(lua_getextraspace(lua));
}

void instructionHook(lua_State *lua, lua_Debug *) {
    Compiler *compiler = context(lua);
    compiler->instructions += 100;
    if (compiler->instructions >= SCRIPT_INSTRUCTIONS_MAX)
        luaL_error(lua, "instruction limit exceeded (100000)");
}

ScriptAction *append(lua_State *lua, ScriptKind kind) {
    if (context(lua)->registering)
        luaL_error(lua, "device actions belong inside hook callbacks, not registration");
    ScriptPlan *plan = context(lua)->plan;
    if (plan->count == SCRIPT_ACTIONS_MAX)
        luaL_error(lua, "action limit exceeded (128)");
    ScriptAction *action = &plan->actions[plan->count++];
    *action = {};
    action->kind = kind;
    return action;
}

bool validChord(lua_State *lua, int index, bool allowModifiers, const char **keysOut,
                size_t *lengthOut) {
    if (lua_type(lua, index) != LUA_TSTRING) return false;
    size_t length = 0;
    const char *keys = lua_tolstring(lua, index, &length);
    uint8_t report[8];
    if (!length || length >= sizeof(ScriptAction::keys) ||
        std::memchr(keys, '\0', length) ||
        (!scriptMediaUsage(keys) && !scriptChordReport(keys, report, allowModifiers)))
        return false;
    *keysOut = keys;
    *lengthOut = length;
    return true;
}

int tap(lua_State *lua) {
    if (lua_gettop(lua) != 1 || lua_type(lua, 1) != LUA_TSTRING)
        return luaL_error(lua, "tap expects exactly one string argument");
    const char *keys = nullptr;
    size_t length = 0;
    if (!validChord(lua, 1, false, &keys, &length))
        return luaL_error(lua, "tap: invalid keyboard chord");
    ScriptAction *action = append(lua, ScriptKind::Tap);
    std::memcpy(action->keys, keys, length + 1);
    return 0;
}

int press(lua_State *lua) {
    if (lua_gettop(lua) != 1 || lua_type(lua, 1) != LUA_TSTRING)
        return luaL_error(lua, "press expects exactly one string argument");
    const char *keys = nullptr;
    size_t length = 0;
    if (!validChord(lua, 1, true, &keys, &length))
        return luaL_error(lua, "press: invalid keyboard chord");
    ScriptAction *action = append(lua, ScriptKind::Press);
    std::memcpy(action->keys, keys, length + 1);
    return 0;
}

int release(lua_State *lua) {
    if (lua_gettop(lua) != 0)
        return luaL_error(lua, "release expects no arguments");
    append(lua, ScriptKind::Release);
    return 0;
}

int wait(lua_State *lua) {
    if (lua_gettop(lua) != 1 || !lua_isinteger(lua, 1))
        return luaL_error(lua, "wait expects exactly one integer argument");
    lua_Integer value = lua_tointeger(lua, 1);
    if (value < 0 || value > 60000)
        return luaL_error(lua, "wait must be in range 0..60000 ms");
    Compiler *compiler = context(lua);
    if (static_cast<uint64_t>(value) > DURATION_MAX - compiler->duration)
        return luaL_error(lua, "total duration limit exceeded (60000 ms)");
    append(lua, ScriptKind::Wait)->value = static_cast<uint32_t>(value);
    compiler->duration += static_cast<uint64_t>(value);
    return 0;
}

int light(lua_State *lua) {
    if (lua_gettop(lua) != 1 || !lua_isinteger(lua, 1))
        return luaL_error(lua, "light expects exactly one integer argument");
    lua_Integer value = lua_tointeger(lua, 1);
    if (value < 0 || value > 127)
        return luaL_error(lua, "light must be in range 0..127");
    append(lua, ScriptKind::Light)->value = static_cast<uint32_t>(value);
    return 0;
}

int type(lua_State *lua) {
    int arguments = lua_gettop(lua);
    if ((arguments != 1 && arguments != 2) || lua_type(lua, 1) != LUA_TSTRING)
        return luaL_error(lua, "type expects a string and optional integer interval");
    lua_Integer interval = 50;
    if (arguments == 2) {
        if (!lua_isinteger(lua, 2))
            return luaL_error(lua, "type interval must be an integer");
        interval = lua_tointeger(lua, 2);
    }
    if (interval < 30 || interval > 60000)
        return luaL_error(lua, "type interval must be in range 30..60000 ms");

    size_t length = 0;
    const char *text = lua_tolstring(lua, 1, &length);
    if (length > sizeof(ScriptAction::text) - 1 ||
        std::memchr(text, '\0', length))
        return luaL_error(lua, "type text must be at most 255 bytes and contain no NUL");
    for (size_t i = 0; i < length; ++i) {
        unsigned char character = static_cast<unsigned char>(text[i]);
        if (!((character >= 32 && character <= 126) || character == '\n' || character == '\t'))
            return luaL_error(lua, "type text only supports printable US ASCII, LF, and TAB");
    }
    Compiler *compiler = context(lua);
    if (length > SCRIPT_TYPE_CHARS_MAX - compiler->typeChars)
        return luaL_error(lua, "total type character limit exceeded (1024)");
    uint64_t duration = length ? static_cast<uint64_t>(length - 1) *
        static_cast<uint64_t>(interval) + 30 : 0;
    if (duration > DURATION_MAX - compiler->duration)
        return luaL_error(lua, "total duration limit exceeded (60000 ms)");

    ScriptAction *action = append(lua, ScriptKind::Type);
    action->value = static_cast<uint32_t>(interval);
    std::memcpy(action->text, text, length);
    compiler->typeChars += static_cast<unsigned>(length);
    compiler->duration += duration;
    return 0;
}

// Lua's instruction hook cannot interrupt a C library loop. Bound both each
// call and cumulative C work, including loops over absent table entries.
void charge(lua_State *lua, size_t work) {
    Compiler *compiler = context(lua);
    if (work > 65536 - compiler->helperWork)
        luaL_error(lua, "instruction limit exceeded (library work)");
    compiler->helperWork += static_cast<unsigned>(work);
}

lua_Integer integer(lua_State *lua, int index) {
    if (!lua_isinteger(lua, index)) luaL_error(lua, "expected integer argument");
    return lua_tointeger(lua, index);
}

double finite(lua_State *lua, int index) {
    if (lua_type(lua, index) != LUA_TNUMBER || !std::isfinite(lua_tonumber(lua, index)))
        luaL_error(lua, "expected finite number");
    return lua_tonumber(lua, index);
}

void arity(lua_State *lua, int low, int high) {
    if (lua_gettop(lua) < low || lua_gettop(lua) > high)
        luaL_error(lua, "invalid argument count");
}

int millis(lua_State *lua) {
    arity(lua, 0, 0);
    if (context(lua)->registering) return luaL_error(lua, "millis is only available inside callbacks");
    // Monotonic snapshots beyond Lua's signed integer range saturate.
    uint64_t value = context(lua)->event->monotonicMs;
    lua_pushinteger(lua, value > static_cast<uint64_t>(LUA_MAXINTEGER)
        ? LUA_MAXINTEGER : static_cast<lua_Integer>(value));
    return 1;
}

int epoch(lua_State *lua) {
    arity(lua, 0, 0);
    if (context(lua)->registering) return luaL_error(lua, "epoch is only available inside callbacks");
    lua_pushinteger(lua, context(lua)->event->epochSeconds);
    return 1;
}

int log(lua_State *lua) {
    arity(lua, 1, 1);
    if (lua_type(lua, 1) != LUA_TSTRING) return luaL_error(lua, "log expects string");
    size_t size = 0;
    const char *text = lua_tolstring(lua, 1, &size);
    if (size > 255 || std::memchr(text, 0, size))
        return luaL_error(lua, "log requires at most 255 bytes and no NUL");
    std::memcpy(append(lua, ScriptKind::Log)->text, text, size);
    return 0;
}

int soundPlay(lua_State *lua) {
    arity(lua, 1, 2);
    if (lua_type(lua, 1) != LUA_TSTRING) return luaL_error(lua, "sound basename must be string");
    size_t size = 0;
    const char *name = lua_tolstring(lua, 1, &size);
    if (!size || size >= MIDI_SFX_PATH_MAX || std::memchr(name, 0, size) ||
        std::strstr(name, ".."))
        return luaL_error(lua, "invalid sound basename");
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (c < 32 || c == 127 || c == '/' || c == '\\')
            return luaL_error(lua, "invalid sound basename");
    }
    double pitch = lua_gettop(lua) == 2 ? finite(lua, 2) : 1.0;
    if (pitch < 0.25 || pitch > 4.0) return luaL_error(lua, "pitch ratio must be 0.25..4");
    ScriptAction *action = append(lua, ScriptKind::SoundPlay);
    std::memcpy(action->text, name, size);
    action->pitchRatio = pitch;
    return 0;
}

int soundStopAll(lua_State *lua) {
    arity(lua, 0, 0);
    append(lua, ScriptKind::SoundStopAll);
    return 0;
}

void identifier(lua_State *lua, int index, char (&out)[64]) {
    if (lua_type(lua, index) != LUA_TSTRING) luaL_error(lua, "target and parameter IDs must be strings");
    size_t size = 0;
    const char *text = lua_tolstring(lua, index, &size);
    if (!size || size >= sizeof(out)) luaL_error(lua, "ID must be 1..63 bytes");
    for (size_t i = 0; i < size; ++i) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        if (c < 33 || c > 126) luaL_error(lua, "IDs must be printable ASCII without spaces");
    }
    memcpy(out, text, size);
}

int meterGet(lua_State *lua) {
    arity(lua, 1, 2);
    auto *compiler = context(lua);
    if (compiler->registering) return luaL_error(lua, "meters.get belongs inside callbacks");
    if (++compiler->meterReads > 32) return luaL_error(lua, "meter read limit exceeded (32)");
    if (!compiler->meters) return luaL_error(lua, "meters.get: audio snapshot unavailable");
    char id[64]{};
    identifier(lua, 1, id);
    lua_Integer channel = 0;
    if (lua_gettop(lua) == 2) {
        if (!lua_isinteger(lua, 2)) return luaL_error(lua, "meter channel must be an integer");
        channel = lua_tointeger(lua, 2);
    }
    if (channel < 0 || channel >= 16) return luaL_error(lua, "meter channel out of range (0..15)");
    for (unsigned i = 0; i < compiler->meters->count; ++i) {
        const auto &node = compiler->meters->nodes[i];
        if (strcmp(node.id, id)) continue;
        if (channel >= node.channels) return luaL_error(lua, "meter channel not present on node");
        const float level = node.levels[channel];
        if (!std::isfinite(level)) return luaL_error(lua, "meter reading is not finite");
        lua_pushnumber(lua, level);
        return 1;
    }
    return luaL_error(lua, "meters.get: unknown routing thing ID");
}

int deviceControl(lua_State *lua) {
    auto kind = static_cast<ScriptKind>(lua_tointeger(lua, lua_upvalueindex(1)));
    int arguments = kind == ScriptKind::EffectToggle ? 1 :
        kind == ScriptKind::EffectSet ? 3 : 2;
    arity(lua, arguments, arguments);
    ScriptAction *action = append(lua, kind);
    identifier(lua, 1, action->target);
    if (kind == ScriptKind::EffectEnable) {
        if (lua_type(lua, 2) != LUA_TBOOLEAN) return luaL_error(lua, "effects.enable expects a boolean");
        action->value = lua_toboolean(lua, 2);
    } else if (kind == ScriptKind::EffectSet) {
        identifier(lua, 2, action->parameter);
        action->number = finite(lua, 3);
    } else if (kind == ScriptKind::VolumeSet || kind == ScriptKind::VolumeAdjust) {
        action->number = finite(lua, 2);
        double minimum = kind == ScriptKind::VolumeAdjust ? -100 : 0;
        if (action->number < minimum || action->number > 100)
            return luaL_error(lua, "volume percentage out of range");
    }
    return 0;
}

int midiData(lua_State *lua, int index, int maximum = 127) {
    lua_Integer value = integer(lua, index);
    if (value < 0 || value > maximum) luaL_error(lua, "MIDI value out of range");
    return static_cast<int>(value);
}

void midiAction(lua_State *lua, int status, int d1, int d2, int size) {
    ScriptAction *action = append(lua, ScriptKind::Midi);
    action->midiBytes[0] = static_cast<uint8_t>(status);
    action->midiBytes[1] = static_cast<uint8_t>(d1);
    action->midiBytes[2] = static_cast<uint8_t>(d2);
    action->midiSize = static_cast<uint8_t>(size);
}

int midiSend(lua_State *lua) {
    arity(lua, 1, 3);
    lua_Integer status = integer(lua, 1);
    int data = -1;
    if (status >= 0x80 && status <= 0xef)
        data = (status & 0xe0) == 0xc0 ? 1 : 2;
    else if (status == 0xf1 || status == 0xf3) data = 1;
    else if (status == 0xf2) data = 2;
    else if (status == 0xf6 || status == 0xf8 || status == 0xfa ||
             status == 0xfb || status == 0xfc || status == 0xfe || status == 0xff) data = 0;
    if (data < 0) return luaL_error(lua, "unsupported MIDI status");
    if (lua_gettop(lua) != data + 1) return luaL_error(lua, "MIDI message length mismatch");
    int d1 = data >= 1 ? midiData(lua, 2) : 0;
    int d2 = data == 2 ? midiData(lua, 3) : 0;
    midiAction(lua, static_cast<int>(status), d1, d2, data + 1);
    return 0;
}

int midiNamed(lua_State *lua) {
    int status = static_cast<int>(lua_tointeger(lua, lua_upvalueindex(1)));
    int arguments = status == 0xc0 ? 2 : 3;
    arity(lua, status == 0x80 ? 2 : arguments, arguments);
    int channel = midiData(lua, 1, 15);
    int d1 = midiData(lua, 2);
    int d2 = lua_gettop(lua) == 3 ? midiData(lua, 3) : 0;
    midiAction(lua, status | channel, d1, d2, status == 0xc0 ? 2 : 3);
    return 0;
}

uint64_t randomBits(Compiler *compiler) {
    uint64_t value = (compiler->randomState += UINT64_C(0x9e3779b97f4a7c15));
    value = (value ^ (value >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)) * UINT64_C(0x94d049bb133111eb);
    return value ^ (value >> 31);
}

uint64_t randomBelow(lua_State *lua, uint64_t span) {
    if (!span) return randomBits(context(lua));
    uint64_t threshold = (uint64_t{0} - span) % span;
    for (int attempt = 0; attempt < 32; ++attempt) {
        uint64_t bits = randomBits(context(lua));
        if (bits >= threshold) return bits % span;
    }
    luaL_error(lua, "random sampling limit exceeded");
    return 0;
}

int randomSeed(lua_State *lua) {
    arity(lua, 1, 1);
    context(lua)->randomState = static_cast<uint64_t>(integer(lua, 1));
    return 0;
}

int random(lua_State *lua) {
    arity(lua, 0, 2);
    if (!lua_gettop(lua)) {
        lua_pushnumber(lua, static_cast<double>(randomBits(context(lua)) >> 11) * 0x1.0p-53);
    } else {
        lua_Integer low = lua_gettop(lua) == 1 ? 1 : integer(lua, 1);
        lua_Integer high = integer(lua, lua_gettop(lua));
        if (low > high) return luaL_error(lua, "empty random interval");
        uint64_t bits = static_cast<uint64_t>(low) +
            randomBelow(lua, static_cast<uint64_t>(high) - static_cast<uint64_t>(low) + 1);
        lua_Integer result;
        static_assert(sizeof(result) == sizeof(bits), "Lua must use 64-bit integers");
        std::memcpy(&result, &bits, sizeof(result));
        lua_pushinteger(lua, result);
    }
    return 1;
}

int choose(lua_State *lua) {
    arity(lua, 1, 1);
    luaL_checktype(lua, 1, LUA_TTABLE);
    size_t count = lua_rawlen(lua, 1);
    if (!count || count > 1024) return luaL_error(lua, "choose requires dense array of 1..1024 items");
    charge(lua, count);
    size_t keys = 0;
    lua_pushnil(lua);
    while (lua_next(lua, 1)) {
        if (!lua_isinteger(lua, -2) || lua_tointeger(lua, -2) < 1 ||
            static_cast<uint64_t>(lua_tointeger(lua, -2)) > count || ++keys > count)
            return luaL_error(lua, "choose requires dense array");
        lua_pop(lua, 1);
    }
    if (keys != count) return luaL_error(lua, "choose requires dense array");
    lua_rawgeti(lua, 1, static_cast<lua_Integer>(randomBelow(lua, count) + 1));
    return 1;
}

int clamp(lua_State *lua) {
    arity(lua, 3, 3);
    double n = finite(lua, 1), low = finite(lua, 2), high = finite(lua, 3);
    if (low > high) return luaL_error(lua, "clamp bounds reversed");
    lua_pushnumber(lua, n < low ? low : (n > high ? high : n));
    return 1;
}

int map(lua_State *lua) {
    arity(lua, 5, 5);
    double n = finite(lua, 1), low = finite(lua, 2), high = finite(lua, 3);
    double outLow = finite(lua, 4), outHigh = finite(lua, 5);
    if (low == high) return luaL_error(lua, "map input range is zero");
    double result = outLow + (n - low) / (high - low) * (outHigh - outLow);
    if (!std::isfinite(result)) return luaL_error(lua, "map result must be finite");
    lua_pushnumber(lua, result);
    return 1;
}

int round(lua_State *lua) {
    arity(lua, 1, 2);
    double n = finite(lua, 1);
    lua_Integer decimals = lua_gettop(lua) == 2 ? integer(lua, 2) : 0;
    if (decimals < -6 || decimals > 6) return luaL_error(lua, "round decimals must be -6..6");
    double scale = std::pow(10.0, static_cast<double>(decimals));
    double result = std::round(n * scale) / scale;
    if (!std::isfinite(result)) return luaL_error(lua, "round result must be finite");
    lua_pushnumber(lua, result);
    return 1;
}

// Each wrapper retains only its single original Lua function as an upvalue.
// No original library table (or excluded function) is reachable by scripts.
enum LibraryOp { Byte, Char, Len, Lower, Upper, Sub, Rep, Reverse, Format,
                 Concat, Insert, Remove, Move, Unpack };

size_t stringSize(lua_State *lua, int index) {
    if (lua_type(lua, index) != LUA_TSTRING) luaL_error(lua, "expected string");
    size_t size = lua_rawlen(lua, index);
    if (size > 4096) luaL_error(lua, "string limit exceeded (4096 bytes)");
    return size;
}

void spanLimit(lua_State *lua, lua_Integer first, lua_Integer last) {
    uint64_t span = last >= first
        ? static_cast<uint64_t>(last) - static_cast<uint64_t>(first) : 0;
    if (last >= first && span >= 1024) luaL_error(lua, "table/string span limit exceeded (1024)");
    charge(lua, last >= first ? static_cast<size_t>(span + 1) : 1);
}

void checkFormat(lua_State *lua, const char *format, size_t length) {
    unsigned fields = 0;
    for (size_t i = 0; i < length; ++i) {
        if (format[i] != '%') continue;
        if (++i == length) luaL_error(lua, "invalid format");
        if (format[i] == '%') continue;
        if (++fields > 16) luaL_error(lua, "format field limit exceeded (16)");
        unsigned flags = 0;
        while (i < length && std::strchr("-+ #0", format[i]) && format[i]) {
            if (++flags > 5) luaL_error(lua, "invalid format flags");
            ++i;
        }
        for (int part = 0; part < 2; ++part) {
            unsigned width = 0;
            while (i < length && format[i] >= '0' && format[i] <= '9') {
                width = width * 10 + static_cast<unsigned>(format[i++] - '0');
                if (width > 256) luaL_error(lua, "format width/precision limit exceeded (256)");
            }
            if (part == 0 && i < length && format[i] == '.') ++i;
            else break;
        }
        if (i == length || !format[i] || !std::strchr("cdiouxXaAeEfgGqs", format[i]))
            luaL_error(lua, "unsupported format");
    }
}

int boundedLibrary(lua_State *lua) {
    int count = lua_gettop(lua);
    LibraryOp op = static_cast<LibraryOp>(lua_tointeger(lua, lua_upvalueindex(2)));
    charge(lua, 1);
    // Lua's integer argument coercions also parse strings in C.
    for (int i = 1; i <= count; ++i)
        if (lua_type(lua, i) == LUA_TSTRING) charge(lua, stringSize(lua, i));
    if (op <= Format) {
        if (op == Char) {
            if (count > 1024) return luaL_error(lua, "string.char argument limit (1024)");
            charge(lua, static_cast<size_t>(count));
        } else {
            size_t size = stringSize(lua, 1);
            charge(lua, size);
            if (op == Byte) {
                arity(lua, 1, 3);
                lua_Integer first = count >= 2 ? integer(lua, 2) : 1;
                lua_Integer last = count >= 3 ? integer(lua, 3) : first;
                // Normalize negatives without negating LUA_MININTEGER.
                if (first < 0) first = first < -static_cast<lua_Integer>(size) ? 1 : first + size + 1;
                if (last < 0) last = last < -static_cast<lua_Integer>(size) ? 0 : last + size + 1;
                if (first < 1) first = 1;
                if (last > static_cast<lua_Integer>(size)) last = static_cast<lua_Integer>(size);
                spanLimit(lua, first, last);
            } else if (op == Rep) {
                arity(lua, 2, 3);
                lua_Integer repeats = integer(lua, 2);
                size_t separator = count == 3 ? stringSize(lua, 3) : 0;
                if (repeats > 4096 || (repeats > 0 &&
                    (size + separator) * static_cast<uint64_t>(repeats) - separator > 4096))
                    return luaL_error(lua, "string.rep limit exceeded (4096 bytes/repetitions)");
                charge(lua, repeats > 0 ? static_cast<size_t>(repeats) : 1);
            } else if (op == Format) {
                checkFormat(lua, lua_tostring(lua, 1), size);
                if (count > 17) return luaL_error(lua, "format argument limit (16)");
                size_t strings = 0;
                for (int i = 2; i <= count; ++i) {
                    if (lua_type(lua, i) == LUA_TSTRING) strings += stringSize(lua, i);
                    else if (lua_type(lua, i) != LUA_TNUMBER && lua_type(lua, i) != LUA_TBOOLEAN &&
                             !lua_isnil(lua, i))
                        return luaL_error(lua, "format only supports scalar arguments");
                }
                if (strings > 4096) return luaL_error(lua, "format string arguments limit (4096 bytes)");
                charge(lua, strings + 4096);
            } else {
                arity(lua, op == Sub ? 2 : 1, op == Sub ? 3 : 1);
            }
        }
    } else {
        luaL_checktype(lua, 1, LUA_TTABLE);
        size_t length = lua_rawlen(lua, 1);
        if (length > 1024) return luaL_error(lua, "table length limit exceeded (1024)");
        if (op == Move) {
            arity(lua, 4, 5);
            lua_Integer first = integer(lua, 2), last = integer(lua, 3);
            lua_Integer destination = integer(lua, 4);
            spanLimit(lua, first, last);
            if (last >= first && destination > LUA_MAXINTEGER - (last - first))
                return luaL_error(lua, "table.move destination overflow");
            if (count == 5) luaL_checktype(lua, 5, LUA_TTABLE);
        } else if (op == Unpack || op == Concat) {
            arity(lua, 1, op == Unpack ? 3 : 4);
            int offset = op == Unpack ? 0 : 1;
            lua_Integer first = count >= 2 + offset ? integer(lua, 2 + offset) : 1;
            lua_Integer last = count >= 3 + offset ? integer(lua, 3 + offset) : static_cast<lua_Integer>(length);
            spanLimit(lua, first, last);
            if (op == Concat) {
                size_t separator = count >= 2 ? stringSize(lua, 2) : 0;
                size_t bytes = last > first ? separator * static_cast<size_t>(last - first) : 0;
                if (bytes > 4096) return luaL_error(lua, "table.concat string limit (4096 bytes)");
                for (lua_Integer i = first; i <= last;) {
                    lua_rawgeti(lua, 1, i);
                    if (lua_type(lua, -1) != LUA_TSTRING && lua_type(lua, -1) != LUA_TNUMBER)
                        return luaL_error(lua, "table.concat requires strings/numbers");
                    size_t size = 0;
                    lua_tolstring(lua, -1, &size);
                    bytes += size;
                    lua_pop(lua, 1);
                    if (bytes > 4096) return luaL_error(lua, "table.concat string limit (4096 bytes)");
                    if (i == last) break;
                    ++i;
                }
                charge(lua, bytes);
            }
        } else {
            arity(lua, op == Insert ? 2 : 1, op == Insert ? 3 : 2);
            if (op == Insert && length == 1024) return luaL_error(lua, "table length limit exceeded (1024)");
            charge(lua, length);
        }
    }
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_insert(lua, 1);
    lua_call(lua, count, LUA_MULTRET);
    if (op <= Format && op != Byte && lua_gettop(lua) && lua_type(lua, 1) == LUA_TSTRING)
        stringSize(lua, 1);
    return lua_gettop(lua);
}

int boundedBase(lua_State *lua) {
    int count = lua_gettop(lua);
    for (int i = 1; i <= count; ++i) {
        if (lua_type(lua, i) == LUA_TSTRING) charge(lua, stringSize(lua, i));
    }
    lua_pushvalue(lua, lua_upvalueindex(1));
    lua_insert(lua, 1);
    lua_call(lua, count, LUA_MULTRET);
    return lua_gettop(lua);
}

int mathFunction(lua_State *lua) {
    const char *name = lua_tostring(lua, lua_upvalueindex(1));
    int count = lua_gettop(lua);
    if (!std::strcmp(name, "type")) {
        arity(lua, 1, 1);
        if (lua_type(lua, 1) != LUA_TNUMBER) lua_pushnil(lua);
        else lua_pushstring(lua, lua_isinteger(lua, 1) ? "integer" : "float");
        return 1;
    }
    if (!std::strcmp(name, "tointeger")) {
        arity(lua, 1, 1);
        if (lua_type(lua, 1) == LUA_TSTRING) charge(lua, stringSize(lua, 1));
        int valid = 0;
        lua_Integer result = lua_tointegerx(lua, 1, &valid);
        if (valid) lua_pushinteger(lua, result); else lua_pushnil(lua);
        return 1;
    }
    if (!std::strcmp(name, "ult")) {
        arity(lua, 2, 2);
        lua_pushboolean(lua, static_cast<lua_Unsigned>(integer(lua, 1)) <
                            static_cast<lua_Unsigned>(integer(lua, 2)));
        return 1;
    }
    bool min = !std::strcmp(name, "min"), max = !std::strcmp(name, "max");
    if (min || max) {
        arity(lua, 1, 1024);
        charge(lua, static_cast<size_t>(count));
        int selected = 1;
        finite(lua, 1);
        for (int i = 2; i <= count; ++i) {
            finite(lua, i);
            if (lua_compare(lua, i, selected, min ? LUA_OPLT : LUA_OPLE) == (min ? 1 : 0))
                selected = i;
        }
        lua_pushvalue(lua, selected);
        return 1;
    }
    bool binary = !std::strcmp(name, "atan") || !std::strcmp(name, "log") || !std::strcmp(name, "fmod");
    arity(lua, 1, binary ? 2 : 1);
    double n = finite(lua, 1), result = 0;
    if (!std::strcmp(name, "abs")) {
        if (lua_isinteger(lua, 1)) {
            lua_Integer value = lua_tointeger(lua, 1);
            lua_pushinteger(lua, value < 0 && value != LUA_MININTEGER ? -value : value);
            return 1;
        }
        result = std::fabs(n);
    } else if (!std::strcmp(name, "floor") || !std::strcmp(name, "ceil")) {
        if (lua_isinteger(lua, 1)) { lua_pushvalue(lua, 1); return 1; }
        result = !std::strcmp(name, "floor") ? std::floor(n) : std::ceil(n);
        if (result >= static_cast<double>(LUA_MININTEGER) && result < -static_cast<double>(LUA_MININTEGER)) {
            lua_pushinteger(lua, static_cast<lua_Integer>(result));
            return 1;
        }
    } else if (!std::strcmp(name, "sqrt")) result = std::sqrt(n);
    else if (!std::strcmp(name, "exp")) result = std::exp(n);
    else if (!std::strcmp(name, "sin")) result = std::sin(n);
    else if (!std::strcmp(name, "cos")) result = std::cos(n);
    else if (!std::strcmp(name, "tan")) result = std::tan(n);
    else if (!std::strcmp(name, "asin")) result = std::asin(n);
    else if (!std::strcmp(name, "acos")) result = std::acos(n);
    else if (!std::strcmp(name, "atan")) result = std::atan2(n, count == 2 ? finite(lua, 2) : 1);
    else if (!std::strcmp(name, "log")) result = count == 2 ? std::log(n) / std::log(finite(lua, 2)) : std::log(n);
    else if (!std::strcmp(name, "deg")) result = n * (180.0 / 3.14159265358979323846);
    else if (!std::strcmp(name, "rad")) result = n * (3.14159265358979323846 / 180.0);
    else if (!std::strcmp(name, "fmod")) {
        arity(lua, 2, 2);
        if (lua_isinteger(lua, 1) && lua_isinteger(lua, 2)) {
            lua_Integer a = integer(lua, 1), b = integer(lua, 2);
            if (!b) return luaL_error(lua, "zero divisor");
            lua_pushinteger(lua, b == -1 ? 0 : a % b);
            return 1;
        }
        result = std::fmod(n, finite(lua, 2));
    } else if (!std::strcmp(name, "modf")) {
        if (lua_isinteger(lua, 1)) { lua_pushvalue(lua, 1); lua_pushnumber(lua, 0); return 2; }
        double integral = 0;
        double fraction = std::modf(n, &integral);
        if (integral >= static_cast<double>(LUA_MININTEGER) && integral < -static_cast<double>(LUA_MININTEGER))
            lua_pushinteger(lua, static_cast<lua_Integer>(integral));
        else lua_pushnumber(lua, integral);
        lua_pushnumber(lua, fraction);
        return 2;
    }
    lua_pushnumber(lua, result);
    return 1;
}

void setFunction(lua_State *lua, const char *name, lua_CFunction function) {
    lua_pushcfunction(lua, function);
    lua_setfield(lua, -2, name);
}

const char *configName(lua_State *lua) {
    size_t length = 0;
    const char *name = luaL_checklstring(lua, 1, &length);
    if (memchr(name, '\0', length)) luaL_error(lua, "invalid configuration name");
    for (const char *allowed : SCRIPT_CONFIG_FILES)
        if (strcmp(name, allowed) == 0) return name;
    luaL_error(lua, "configuration file is not on the allowlist; use config.list()");
    return nullptr;
}

int configList(lua_State *lua) {
    arity(lua, 0, 0);
    lua_newtable(lua);
    unsigned index = 1;
    for (const char *name : SCRIPT_CONFIG_FILES) {
        lua_pushstring(lua, name);
        lua_rawseti(lua, -2, index++);
    }
    return 1;
}

int configRead(lua_State *lua) {
    arity(lua, 1, 1);
    if (context(lua)->registering) return luaL_error(lua, "configuration reads belong inside callbacks");
    const char *name = configName(lua);
    ConfigStore *store = context(lua)->config;
    if (!store) return luaL_error(lua, "configuration access requires the device runtime");
    if (++context(lua)->configReads > 4) return luaL_error(lua, "configuration read limit exceeded (4)");
    auto *buffer = static_cast<char *>(lua_newuserdatauv(lua, SCRIPT_CONFIG_READ_MAX + 1, 0));
    size_t length = 0;
    if (store->readScriptConfig(name, buffer, SCRIPT_CONFIG_READ_MAX + 1, &length) != RET_OK)
        return luaL_error(lua, "config.read failed: file unavailable, invalid, or larger than 64 KiB");
    charge(lua, 100);
    lua_pushlstring(lua, buffer, length);
    lua_remove(lua, -2);
    return 1;
}

int configLines(lua_State *lua) {
    configRead(lua);
    size_t length = 0;
    const char *text = lua_tolstring(lua, -1, &length);
    lua_newtable(lua);
    unsigned index = 1;
    for (size_t start = 0; start < length;) {
        size_t end = start;
        while (end < length && text[end] != '\n') ++end;
        size_t lineLength = end - start;
        if (lineLength && text[end - 1] == '\r') --lineLength;
        if (lineLength > 4096 || index > 1024)
            return luaL_error(lua, "config.lines limit exceeded (1024 lines, 4096 bytes per line)");
        lua_pushlstring(lua, text + start, lineLength);
        lua_rawseti(lua, -2, index++);
        start = end + 1;
    }
    return 1;
}

int configWrite(lua_State *lua) {
    arity(lua, 2, 2);
    if (context(lua)->registering) return luaL_error(lua, "configuration writes belong inside callbacks");
    const char *name = configName(lua);
    if (!context(lua)->configWriteAllowed)
        return luaL_error(lua, "configuration writes require acknowledgment when saving the script");
    size_t length = 0;
    const char *text = luaL_checklstring(lua, 2, &length);
    if (!scriptConfigTextValid(text, length))
        return luaL_error(lua, "config.write text must be at most 4096 bytes with no NUL or non-whitespace control characters");
    auto *plan = context(lua)->plan;
    if (length + 1 > SCRIPT_CONFIG_PLAN_MAX - plan->configBytes)
        return luaL_error(lua, "configuration write payload limit exceeded (8192 bytes)");
    auto *action = append(lua, ScriptKind::ConfigWrite);
    snprintf(action->target, sizeof(action->target), "%s", name);
    action->value = plan->configBytes;
    action->number = static_cast<double>(length);
    memcpy(plan->configText + plan->configBytes, text, length);
    plan->configText[plan->configBytes + length] = '\0';
    plan->configBytes += static_cast<unsigned>(length + 1);
    return 0;
}

void libraries(lua_State *lua) {
    // Open base privately, then replace the global environment with a whitelist.
    luaopen_base(lua);
    lua_newtable(lua);
    const char *base[] = {"assert", "error", "ipairs", "pairs", "next", "select", "tonumber", "tostring"};
    for (const char *name : base) {
        lua_getfield(lua, -2, name);
        lua_pushcclosure(lua, boundedBase, 1);
        lua_setfield(lua, -2, name);
    }
    lua_getfield(lua, -2, "type");
    lua_setfield(lua, -2, "value_type");
    lua_pushvalue(lua, -1);
    lua_rawseti(lua, LUA_REGISTRYINDEX, LUA_RIDX_GLOBALS);
    lua_pop(lua, 2);

    luaopen_string(lua);
    lua_newtable(lua);
    const char *strings[] = {"byte", "char", "len", "lower", "upper", "sub", "rep", "reverse", "format"};
    for (int i = 0; i < 9; ++i) {
        lua_getfield(lua, -2, strings[i]);
        lua_pushinteger(lua, i);
        lua_pushcclosure(lua, boundedLibrary, 2);
        lua_setfield(lua, -2, strings[i]);
    }
    lua_pushliteral(lua, "");
    lua_getmetatable(lua, -1);
    lua_pushvalue(lua, -3);
    lua_setfield(lua, -2, "__index");
    const char *arithmetic[] = {"__add", "__sub", "__mul", "__mod", "__pow", "__div", "__idiv", "__unm"};
    for (const char *name : arithmetic) {
        lua_getfield(lua, -1, name);
        if (lua_isfunction(lua, -1)) {
            lua_pushcclosure(lua, boundedBase, 1);
            lua_setfield(lua, -2, name);
        } else lua_pop(lua, 1);
    }
    lua_pop(lua, 2);
    lua_setglobal(lua, "string");
    lua_pop(lua, 1);

    luaopen_table(lua);
    lua_newtable(lua);
    const char *tables[] = {"concat", "insert", "remove", "move", "unpack"};
    for (int i = 0; i < 5; ++i) {
        lua_getfield(lua, -2, tables[i]);
        lua_pushinteger(lua, Concat + i);
        lua_pushcclosure(lua, boundedLibrary, 2);
        lua_setfield(lua, -2, tables[i]);
    }
    lua_setglobal(lua, "table");
    lua_pop(lua, 1);

    // luaopen_math seeds its PRNG from time/address data; never call it.
    lua_newtable(lua);
    const char *math[] = {"abs", "acos", "asin", "atan", "ceil", "cos", "deg", "exp", "floor",
        "fmod", "log", "max", "min", "modf", "rad", "sin", "sqrt", "tan", "tointeger", "type", "ult"};
    for (const char *name : math) {
        lua_pushstring(lua, name);
        lua_pushcclosure(lua, mathFunction, 1);
        lua_setfield(lua, -2, name);
    }
    lua_pushnumber(lua, 3.14159265358979323846); lua_setfield(lua, -2, "pi");
    lua_pushnumber(lua, HUGE_VAL); lua_setfield(lua, -2, "huge");
    lua_pushinteger(lua, LUA_MININTEGER); lua_setfield(lua, -2, "mininteger");
    lua_pushinteger(lua, LUA_MAXINTEGER); lua_setfield(lua, -2, "maxinteger");
    setFunction(lua, "random", random);
    setFunction(lua, "randomseed", randomSeed);
    lua_setglobal(lua, "math");

    lua_newtable(lua);
    setFunction(lua, "play", soundPlay);
    setFunction(lua, "stop_all", soundStopAll);
    lua_setglobal(lua, "sound");
    lua_newtable(lua);
    setFunction(lua, "send", midiSend);
    const char *named[] = {"note_on", "note_off", "cc", "program"};
    const int statuses[] = {0x90, 0x80, 0xb0, 0xc0};
    for (int i = 0; i < 4; ++i) {
        lua_pushinteger(lua, statuses[i]);
        lua_pushcclosure(lua, midiNamed, 1);
        lua_setfield(lua, -2, named[i]);
    }
    lua_setglobal(lua, "midi");
    lua_newtable(lua);
    const char *effectNames[] = {"enable", "toggle", "set"};
    const ScriptKind effectKinds[] = {ScriptKind::EffectEnable, ScriptKind::EffectToggle,
                                          ScriptKind::EffectSet};
    for (unsigned i = 0; i < 3; ++i) {
        lua_pushinteger(lua, static_cast<int>(effectKinds[i]));
        lua_pushcclosure(lua, deviceControl, 1);
        lua_setfield(lua, -2, effectNames[i]);
    }
    lua_setglobal(lua, "effects");
    lua_newtable(lua);
    lua_pushinteger(lua, static_cast<int>(ScriptKind::VolumeSet));
    lua_pushcclosure(lua, deviceControl, 1);
    lua_setfield(lua, -2, "set");
    lua_pushinteger(lua, static_cast<int>(ScriptKind::VolumeAdjust));
    lua_pushcclosure(lua, deviceControl, 1);
    lua_setfield(lua, -2, "adjust");
    lua_setglobal(lua, "volume");
    lua_newtable(lua);
    lua_pushcfunction(lua, meterGet);
    lua_setfield(lua, -2, "get");
    lua_setglobal(lua, "meters");
    const luaL_Reg helpers[] = {{"millis", millis}, {"epoch", epoch}, {"log", log}, {"choose", choose},
        {"clamp", clamp}, {"map", map}, {"round", round}, {nullptr, nullptr}};
    for (const luaL_Reg *helper = helpers; helper->name; ++helper) {
        lua_pushcfunction(lua, helper->func);
        lua_setglobal(lua, helper->name);
    }
}

bool checkedLength(const char *text, size_t capacity, size_t &length) {
    length = strnlen(text, capacity);
    return length < capacity;
}

bool validateState(const ScriptState &state) {
    if (state.count > SCRIPT_STATE_ENTRIES_MAX) return false;
    for (unsigned i = 0; i < state.count; ++i) {
        const ScriptStateEntry &entry = state.entries[i];
        size_t keyLength = 0;
        if (!checkedLength(entry.key, sizeof(entry.key), keyLength) || !keyLength) return false;
        if (entry.kind == ScriptValueKind::Number) {
            if (!std::isfinite(entry.numberValue)) return false;
            if (entry.numberIsInteger && entry.numberValue != static_cast<double>(entry.integerValue))
                return false;
        } else if (entry.kind == ScriptValueKind::String) {
            size_t valueLength = 0;
            if (!checkedLength(entry.stringValue, sizeof(entry.stringValue), valueLength)) return false;
        } else if (entry.kind != ScriptValueKind::Boolean) {
            return false;
        }
        for (unsigned j = 0; j < i; ++j) {
            if (std::strcmp(entry.key, state.entries[j].key) == 0) return false;
        }
    }
    return true;
}

void pushScalar(lua_State *lua, const ScriptStateEntry &entry) {
    switch (entry.kind) {
    case ScriptValueKind::Boolean: lua_pushboolean(lua, entry.booleanValue); break;
    case ScriptValueKind::Number:
        if (entry.numberIsInteger) lua_pushinteger(lua, entry.integerValue);
        else lua_pushnumber(lua, entry.numberValue);
        break;
    case ScriptValueKind::String: lua_pushstring(lua, entry.stringValue); break;
    }
}

const char *scalarKey(lua_State *lua, int index) {
    if (lua_type(lua, index) != LUA_TSTRING) luaL_error(lua, "key must be a string");
    size_t length = 0;
    const char *key = lua_tolstring(lua, index, &length);
    if (!length || length > SCRIPT_STATE_KEY_MAX || memchr(key, 0, length))
        luaL_error(lua, "key must be 1..31 bytes with no NUL");
    return key;
}

ScriptStateEntry scalar(lua_State *lua, int index, const char *key) {
    ScriptStateEntry entry{};
    snprintf(entry.key, sizeof(entry.key), "%s", key);
    switch (lua_type(lua, index)) {
    case LUA_TBOOLEAN:
        entry.kind = ScriptValueKind::Boolean;
        entry.booleanValue = lua_toboolean(lua, index); break;
    case LUA_TNUMBER:
        entry.kind = ScriptValueKind::Number;
        entry.numberValue = finite(lua, index);
        entry.numberIsInteger = lua_isinteger(lua, index);
        if (entry.numberIsInteger) entry.integerValue = lua_tointeger(lua, index);
        break;
    case LUA_TSTRING: {
        size_t length = 0;
        const char *value = lua_tolstring(lua, index, &length);
        if (length > SCRIPT_STATE_STRING_MAX || memchr(value, 0, length))
            luaL_error(lua, "scalar strings must be at most 127 bytes with no NUL");
        entry.kind = ScriptValueKind::String;
        memcpy(entry.stringValue, value, length); break;
    }
    default: luaL_error(lua, "value must be a boolean, finite number, or string");
    }
    return entry;
}

void persistentAccess(lua_State *lua) {
    if (context(lua)->registering) luaL_error(lua, "persistent storage belongs inside callbacks");
    if (!context(lua)->scriptId) luaL_error(lua, "persistent storage requires a stable script ID");
}
const char *storageName(lua_State *lua) {
    if (lua_type(lua, 1) != LUA_TSTRING) luaL_error(lua, "storage filename must be a string");
    size_t length = 0;
    const char *name = lua_tolstring(lua, 1, &length);
    if (memchr(name, 0, length) || !ScriptStorage::validName(name))
        luaL_error(lua, "invalid storage filename");
    return name;
}
int storageRead(lua_State *lua) {
    arity(lua, 1, 1); persistentAccess(lua);
    const char *name = storageName(lua);
    Compiler *compiler = context(lua);
    if (++compiler->storageReads > 32) return luaL_error(lua, "storage read limit exceeded (32)");
    for (unsigned i = compiler->plan->count; i > 0; --i) {
        const auto &action = compiler->plan->actions[i - 1];
        if ((action.kind == ScriptKind::StorageWrite || action.kind == ScriptKind::StorageRemove) &&
            !strcmp(action.target, name)) {
            if (action.kind == ScriptKind::StorageRemove) lua_pushnil(lua);
            else lua_pushlstring(lua, compiler->plan->configText + action.value, static_cast<size_t>(action.number));
            return 1;
        }
    }
    int result = ScriptStorage(compiler->scriptId).readFile(name, compiler->storageText);
    int saved = errno;
    charge(lua, 100);
    if (result != RET_OK) {
        if (saved == ENOENT) { lua_pushnil(lua); return 1; }
        return luaL_error(lua, "storage.read failed");
    }
    lua_pushlstring(lua, compiler->storageText.data(), compiler->storageText.size());
    return 1;
}
int storageWrite(lua_State *lua) {
    arity(lua, 2, 2); persistentAccess(lua);
    const char *name = storageName(lua);
    if (lua_type(lua, 2) != LUA_TSTRING) return luaL_error(lua, "storage.write text must be a string");
    size_t length = 0;
    const char *text = lua_tolstring(lua, 2, &length);
    if (length > SCRIPT_STORAGE_FILE_MAX) return luaL_error(lua, "storage.write exceeds 4096 bytes");
    auto *plan = context(lua)->plan;
    if (length + 1 > SCRIPT_CONFIG_PLAN_MAX - plan->configBytes)
        return luaL_error(lua, "storage write payload limit exceeded (8192 bytes)");
    auto *action = append(lua, ScriptKind::StorageWrite);
    snprintf(action->target, sizeof(action->target), "%s", name);
    action->value = plan->configBytes;
    action->number = static_cast<double>(length);
    memcpy(plan->configText + plan->configBytes, text, length);
    plan->configText[plan->configBytes + length] = 0;
    plan->configBytes += static_cast<unsigned>(length + 1);
    return 0;
}
int storageRemove(lua_State *lua) {
    arity(lua, 1, 1); persistentAccess(lua);
    const char *name = storageName(lua);
    snprintf(append(lua, ScriptKind::StorageRemove)->target, sizeof(ScriptAction::target), "%s", name);
    return 0;
}
int storageList(lua_State *lua) {
    arity(lua, 0, 0); persistentAccess(lua);
    auto *compiler = context(lua);
    if (++compiler->storageReads > 32) return luaL_error(lua, "storage read limit exceeded (32)");
    int result = ScriptStorage(compiler->scriptId).listFiles(compiler->storageNames);
    if (result != RET_OK) return luaL_error(lua, "storage.list failed");
    for (unsigned i = 0; i < compiler->plan->count; ++i) {
        const auto &action = compiler->plan->actions[i];
        auto &names = compiler->storageNames;
        if (action.kind == ScriptKind::StorageRemove)
            names.erase(std::remove(names.begin(), names.end(), action.target), names.end());
        else if (action.kind == ScriptKind::StorageWrite &&
                 std::find(names.begin(), names.end(), action.target) == names.end()) names.emplace_back(action.target);
    }
    if (compiler->storageNames.size() > SCRIPT_STORAGE_FILES_MAX)
        return luaL_error(lua, "storage file count limit exceeded (32)");
    std::sort(compiler->storageNames.begin(), compiler->storageNames.end());
    charge(lua, 100);
    lua_createtable(lua, static_cast<int>(compiler->storageNames.size()), 0);
    unsigned index = 1;
    for (const auto &name : compiler->storageNames) {
        lua_pushstring(lua, name.c_str()); lua_rawseti(lua, -2, index++);
    }
    return 1;
}
void loadSettings(lua_State *lua) {
    persistentAccess(lua);
    auto *compiler = context(lua);
    if (!compiler->settingsLoaded) {
        if (ScriptStorage(compiler->scriptId).readSettings(compiler->settings) != RET_OK)
            luaL_error(lua, "script_config read failed");
        compiler->settingsLoaded = true;
    }
}
int scriptConfigGet(lua_State *lua) {
    arity(lua, 1, 2);
    const char *key = scalarKey(lua, 1);
    if (lua_gettop(lua) == 2 && !lua_isnil(lua, 2)) scalar(lua, 2, key);
    loadSettings(lua);
    auto *compiler = context(lua);
    for (unsigned i = 0; i < compiler->settings.count; ++i)
        if (!strcmp(compiler->settings.entries[i].key, key)) {
            pushScalar(lua, compiler->settings.entries[i]); return 1;
        }
    if (lua_gettop(lua) == 2) lua_pushvalue(lua, 2);
    else {
        for (unsigned i = 0; i < compiler->ui.count; ++i)
            if (!strcmp(compiler->ui.controls[i].key, key) && compiler->ui.controls[i].kind != ScriptUiKind::Button) {
                pushScalar(lua, compiler->ui.controls[i].defaultValue); return 1;
            }
        lua_pushnil(lua);
    }
    return 1;
}
int scriptConfigSet(lua_State *lua) {
    arity(lua, 2, 2);
    const char *key = scalarKey(lua, 1);
    loadSettings(lua);
    auto *compiler = context(lua);
    bool remove = lua_isnil(lua, 2);
    ScriptStateEntry entry{};
    snprintf(entry.key, sizeof(entry.key), "%s", key);
    if (!remove) entry = scalar(lua, 2, key);
    for (unsigned i = 0; i < compiler->ui.count; ++i)
        if (!strcmp(compiler->ui.controls[i].key, key) &&
            (!remove && !scriptUiValueValid(compiler->ui.controls[i], entry)))
            return luaL_error(lua, "script_config value does not match UI declaration");
    unsigned index = 0;
    auto &settings = compiler->settings;
    while (index < settings.count && strcmp(settings.entries[index].key, key)) ++index;
    if (remove) {
        if (index < settings.count) settings.entries[index] = settings.entries[--settings.count];
    } else {
        if (index == settings.count) {
            if (settings.count == SCRIPT_STATE_ENTRIES_MAX)
                return luaL_error(lua, "script_config entry limit exceeded (32)");
            ++settings.count;
        }
        settings.entries[index] = entry;
    }
    auto *action = append(lua, ScriptKind::ScriptConfigSet);
    snprintf(action->target, sizeof(action->target), "%s", key);
    action->setting = entry;
    action->value = remove ? 0 : 1;
    return 0;
}
ScriptUiControl *uiControl(lua_State *lua) {
    auto *compiler = context(lua);
    if (!compiler->hookMode || !compiler->registering)
        luaL_error(lua, "UI declarations belong at callback script top level");
    const char *key = scalarKey(lua, 1);
    for (unsigned i = 0; i < compiler->ui.count; ++i)
        if (!strcmp(compiler->ui.controls[i].key, key)) luaL_error(lua, "duplicate UI key");
    if (compiler->ui.count == SCRIPT_UI_MAX) luaL_error(lua, "UI control limit exceeded (16)");
    auto *control = &compiler->ui.controls[compiler->ui.count++];
    *control = {};
    snprintf(control->key, sizeof(control->key), "%s", key);
    return control;
}
void uiLabel(lua_State *lua, int index, ScriptUiControl &control) {
    if (lua_type(lua, index) != LUA_TSTRING) luaL_error(lua, "UI label must be a string");
    size_t length = 0;
    const char *label = lua_tolstring(lua, index, &length);
    if (!length || length >= sizeof(control.label) || memchr(label, 0, length))
        luaL_error(lua, "UI label must be 1..63 bytes with no NUL");
    memcpy(control.label, label, length);
}
int uiButton(lua_State *lua) {
    arity(lua, 2, 2);
    auto *control = uiControl(lua);
    control->kind = ScriptUiKind::Button;
    uiLabel(lua, 2, *control);
    return 0;
}
int uiOption(lua_State *lua) {
    arity(lua, 2, 2);
    if (lua_type(lua, 2) != LUA_TTABLE) return luaL_error(lua, "ui.option expects a declaration table");
    auto *control = uiControl(lua);
    lua_pushnil(lua);
    while (lua_next(lua, 2)) {
        if (lua_type(lua, -2) != LUA_TSTRING) return luaL_error(lua, "unsupported UI declaration field");
        size_t length = 0;
        const char *field = lua_tolstring(lua, -2, &length);
        if (memchr(field, 0, length) || (strcmp(field, "label") && strcmp(field, "type") &&
            strcmp(field, "default") && strcmp(field, "min") && strcmp(field, "max")))
            return luaL_error(lua, "unsupported UI declaration field");
        lua_pop(lua, 1);
    }
    lua_getfield(lua, 2, "label"); uiLabel(lua, -1, *control); lua_pop(lua, 1);
    lua_getfield(lua, 2, "type");
    if (lua_type(lua, -1) != LUA_TSTRING) return luaL_error(lua, "UI type must be boolean, number, or string");
    size_t length = 0;
    const char *kind = lua_tolstring(lua, -1, &length);
    if (length == 7 && !memcmp(kind, "boolean", 7)) control->kind = ScriptUiKind::Boolean;
    else if (length == 6 && !memcmp(kind, "number", 6)) control->kind = ScriptUiKind::Number;
    else if (length == 6 && !memcmp(kind, "string", 6)) control->kind = ScriptUiKind::String;
    else return luaL_error(lua, "UI type must be boolean, number, or string");
    lua_pop(lua, 1);
    lua_getfield(lua, 2, "default"); control->defaultValue = scalar(lua, -1, control->key); lua_pop(lua, 1);
    lua_getfield(lua, 2, "min");
    if (!lua_isnil(lua, -1)) { control->hasMin = true; control->min = finite(lua, -1); }
    lua_pop(lua, 1);
    lua_getfield(lua, 2, "max");
    if (!lua_isnil(lua, -1)) { control->hasMax = true; control->max = finite(lua, -1); }
    lua_pop(lua, 1);
    if ((control->kind != ScriptUiKind::Number && (control->hasMin || control->hasMax)) ||
        (control->hasMin && control->hasMax && control->min > control->max) ||
        !scriptUiValueValid(*control, control->defaultValue))
        return luaL_error(lua, "UI default or bounds do not match its type");
    return 0;
}

void pushState(lua_State *lua, const ScriptState &state) {
    lua_createtable(lua, 0, static_cast<int>(state.count));
    for (unsigned i = 0; i < state.count; ++i) {
        const ScriptStateEntry &entry = state.entries[i];
        lua_pushstring(lua, entry.key);
        switch (entry.kind) {
        case ScriptValueKind::Boolean:
            lua_pushboolean(lua, entry.booleanValue);
            break;
        case ScriptValueKind::Number:
            if (entry.numberIsInteger) lua_pushinteger(lua, entry.integerValue);
            else lua_pushnumber(lua, entry.numberValue);
            break;
        case ScriptValueKind::String:
            lua_pushstring(lua, entry.stringValue);
            break;
        }
        lua_rawset(lua, -3);
    }
    lua_setglobal(lua, "state");
}

void pushEvent(lua_State *lua, const ScriptEvent &event) {
    lua_createtable(lua, 0, 4);
    lua_pushinteger(lua, event.channel);
    lua_setfield(lua, -2, "channel");
    lua_pushinteger(lua, event.note);
    lua_setfield(lua, -2, "note");
    lua_pushinteger(lua, event.velocity);
    lua_setfield(lua, -2, "velocity");
    lua_pushboolean(lua, event.pressed);
    lua_setfield(lua, -2, "pressed");
    lua_pushboolean(lua, event.active);
    lua_setfield(lua, -2, "active");
    lua_pushstring(lua, scriptEventName(event.kind));
    lua_setfield(lua, -2, "type");
    lua_pushinteger(lua, event.note);
    lua_setfield(lua, -2, "cc");
    lua_pushinteger(lua, event.velocity);
    lua_setfield(lua, -2, "value");
    lua_pushstring(lua, event.sound);
    lua_setfield(lua, -2, "sound");
    lua_pushstring(lua, event.device);
    lua_setfield(lua, -2, "device");
    lua_pushstring(lua, event.key);
    lua_setfield(lua, -2, "key");
    if (event.kind == ScriptEventKind::Ui || event.kind == ScriptEventKind::ConfigChange) {
        if (event.hasValue) pushScalar(lua, event.value);
        else lua_pushnil(lua);
        lua_setfield(lua, -2, "value");
    }
    const bool audio = event.kind == ScriptEventKind::SoundStart || event.kind == ScriptEventKind::SoundFinish;
    const bool device = event.kind == ScriptEventKind::DeviceConnect || event.kind == ScriptEventKind::DeviceDisconnect;
    const bool system = event.kind == ScriptEventKind::Boot || event.kind == ScriptEventKind::Timer ||
                        event.kind == ScriptEventKind::DeviceActive;
    const bool web = event.fromWeb || event.kind == ScriptEventKind::Ui || event.kind == ScriptEventKind::ConfigChange;
    lua_pushstring(lua, (event.kind == ScriptEventKind::BluetoothChange ||
                        event.kind == ScriptEventKind::BluetoothMediaKey) ? "bluetooth" :
                   system ? "system" : web ? "web" : audio ? "audio" : device ? "device" : "midi");
    lua_setfield(lua, -2, "source");
    lua_setglobal(lua, "event");
}

void exportState(lua_State *lua, ScriptState *result) {
    lua_getglobal(lua, "state");
    if (lua_type(lua, -1) != LUA_TTABLE)
        luaL_error(lua, "state must remain a table");
    ScriptState exported{};
    lua_pushnil(lua);
    while (lua_next(lua, -2) != 0) {
        if (lua_type(lua, -2) != LUA_TSTRING)
            luaL_error(lua, "state keys must be strings");
        size_t keyLength = 0;
        const char *key = lua_tolstring(lua, -2, &keyLength);
        if (!keyLength || keyLength > SCRIPT_STATE_KEY_MAX ||
            std::memchr(key, '\0', keyLength))
            luaL_error(lua, "state keys must be 1..31 bytes");
        if (exported.count == SCRIPT_STATE_ENTRIES_MAX)
            luaL_error(lua, "state entry limit exceeded (32)");
        ScriptStateEntry &entry = exported.entries[exported.count];
        std::memcpy(entry.key, key, keyLength);
        int valueType = lua_type(lua, -1);
        if (valueType == LUA_TBOOLEAN) {
            entry.kind = ScriptValueKind::Boolean;
            entry.booleanValue = lua_toboolean(lua, -1) != 0;
        } else if (valueType == LUA_TNUMBER) {
            entry.numberValue = lua_tonumber(lua, -1);
            if (!std::isfinite(entry.numberValue))
                luaL_error(lua, "state numbers must be finite");
            entry.kind = ScriptValueKind::Number;
            entry.numberIsInteger = lua_isinteger(lua, -1);
            if (entry.numberIsInteger) entry.integerValue = lua_tointeger(lua, -1);
        } else if (valueType == LUA_TSTRING) {
            size_t valueLength = 0;
            const char *value = lua_tolstring(lua, -1, &valueLength);
            if (valueLength >= sizeof(entry.stringValue) ||
                std::memchr(value, '\0', valueLength))
                luaL_error(lua, "state strings must be at most 127 bytes and contain no NUL");
            std::memcpy(entry.stringValue, value, valueLength);
            entry.kind = ScriptValueKind::String;
        } else {
            luaL_error(lua, "state values must be booleans, numbers, or strings");
        }
        ++exported.count;
        lua_pop(lua, 1);
    }
    *result = exported;
}

int registerHook(lua_State *lua) {
    arity(lua, 2, 3);
    Compiler *compiler = context(lua);
    if (!compiler->hookMode) return luaL_error(lua, "hook requires Lua callbacks mode");
    if (!compiler->registering) return luaL_error(lua, "hooks must be registered at script top level");
    const int callback = lua_gettop(lua);
    if (lua_type(lua, 1) != LUA_TSTRING || lua_type(lua, callback) != LUA_TFUNCTION)
        return luaL_error(lua, "hook expects an event name and a function");
    size_t length = 0;
    const char *name = lua_tolstring(lua, 1, &length);
    unsigned kind = 0;
    for (; kind < SCRIPT_EVENT_COUNT; ++kind) {
        const char *candidate = scriptEventName(static_cast<ScriptEventKind>(kind));
        if (length == strlen(candidate) && !memcmp(name, candidate, length)) break;
    }
    if (kind == SCRIPT_EVENT_COUNT) return luaL_error(lua, "unknown hook event name");
    char device[64]{};
    if (kind == static_cast<unsigned>(ScriptEventKind::DeviceActive)) {
        if (callback != 3) return luaL_error(lua, "device_active expects device ID and callback");
        identifier(lua, 2, device);
        unsigned i = 0;
        for (; i < compiler->activity.count; ++i)
            if (!strcmp(compiler->activity.devices[i].id, device)) break;
        if (i == compiler->activity.count) {
            if (i == SCRIPT_ACTIVITY_MAX) return luaL_error(lua, "device_active device limit exceeded (8)");
            strcpy(compiler->activity.devices[i].id, device);
            ++compiler->activity.count;
        }
    } else if (callback != 2) return luaL_error(lua, "hook expects event name and callback");
    if (compiler->hookCount == SCRIPT_HOOKS_MAX)
        return luaL_error(lua, "hook limit exceeded (32)");
    lua_rawgetp(lua, LUA_REGISTRYINDEX, &hooksRegistryKey);
    lua_createtable(lua, 2, 0);
    lua_pushinteger(lua, kind);
    lua_rawseti(lua, -2, 1);
    lua_pushvalue(lua, callback);
    lua_rawseti(lua, -2, 2);
    lua_pushstring(lua, device);
    lua_rawseti(lua, -2, 3);
    lua_rawseti(lua, -2, ++compiler->hookCount);
    compiler->hookMask |= static_cast<uint16_t>(1U << kind);
    return 0;
}

// Setup, parsing, execution, and state export all run inside lua_pcall so
// allocation failures cannot reach Lua's panic handler.
int evaluate(lua_State *lua) {
    Compiler *compiler = context(lua);
    libraries(lua);
    scriptRegisterUtilities(lua);
    lua_pushcfunction(lua, registerHook);
    lua_setglobal(lua, "hook");
    lua_newtable(lua);
    setFunction(lua, "list", configList);
    setFunction(lua, "read", configRead);
    setFunction(lua, "lines", configLines);
    setFunction(lua, "write", configWrite);
    lua_setglobal(lua, "config");
    lua_newtable(lua);
    setFunction(lua, "read", storageRead);
    setFunction(lua, "write", storageWrite);
    setFunction(lua, "remove", storageRemove);
    setFunction(lua, "list", storageList);
    lua_setglobal(lua, "storage");
    lua_newtable(lua);
    setFunction(lua, "get", scriptConfigGet);
    setFunction(lua, "set", scriptConfigSet);
    lua_setglobal(lua, "script_config");
    lua_newtable(lua);
    setFunction(lua, "option", uiOption);
    setFunction(lua, "button", uiButton);
    lua_setglobal(lua, "ui");
    lua_pushcfunction(lua, tap);
    lua_setglobal(lua, "tap");
    lua_pushcfunction(lua, wait);
    lua_setglobal(lua, "wait");
    lua_pushcfunction(lua, light);
    lua_setglobal(lua, "light");
    lua_pushcfunction(lua, press);
    lua_setglobal(lua, "press");
    lua_pushcfunction(lua, release);
    lua_setglobal(lua, "release");
    lua_pushcfunction(lua, type);
    lua_setglobal(lua, "type");
    if (!compiler->syntaxOnly && !compiler->hookMode) {
        pushEvent(lua, *compiler->event);
        pushState(lua, *compiler->initialState);
    }
    int status = luaL_loadbufferx(lua, compiler->source, compiler->length, "script", "t");
    if (status != LUA_OK) return lua_error(lua);
    if (compiler->syntaxOnly) {
        lua_pop(lua, 1);
        return 0;
    }
    if (compiler->hookMode) {
        lua_newtable(lua);
        lua_rawsetp(lua, LUA_REGISTRYINDEX, &hooksRegistryKey);
        compiler->registering = true;
    }
    lua_call(lua, 0, 0);
    if (compiler->hookMode) {
        if (!compiler->hookCount && !compiler->inspectedUi)
            return luaL_error(lua, "callback scripts must register at least one hook");
        compiler->registering = false;
        if (compiler->inspectedUi) {
            *compiler->inspectedUi = compiler->ui;
            return 0;
        }
        if (compiler->discoveredMask) {
            *compiler->discoveredMask = compiler->hookMask;
            if (compiler->discoveredActivity) *compiler->discoveredActivity = compiler->activity;
            return 0;
        }
        if (compiler->expectedHookMask && compiler->expectedHookMask != compiler->hookMask)
            return luaL_error(lua, "hook subscriptions changed; register hooks unconditionally");
        pushEvent(lua, *compiler->event);
        pushState(lua, *compiler->initialState);
        lua_rawgetp(lua, LUA_REGISTRYINDEX, &hooksRegistryKey);
        for (unsigned i = 1; i <= compiler->hookCount; ++i) {
            lua_rawgeti(lua, -1, i);
            lua_rawgeti(lua, -1, 1);
            bool matches = lua_tointeger(lua, -1) == static_cast<unsigned>(compiler->event->kind);
            lua_pop(lua, 1);
            if (matches && compiler->event->kind == ScriptEventKind::DeviceActive) {
                lua_rawgeti(lua, -1, 3);
                matches = !strcmp(lua_tostring(lua, -1), compiler->event->device);
                lua_pop(lua, 1);
            }
            if (matches) {
                lua_rawgeti(lua, -1, 2);
                lua_getglobal(lua, "event");
                lua_call(lua, 1, 0);
            }
            lua_pop(lua, 1);
        }
        lua_pop(lua, 1);
    }
    exportState(lua, compiler->state);
    return 0;
}

void writeError(char *error, size_t size, const char *message) {
    if (error && size) std::snprintf(error, size, "%s", message);
}

bool sourceLength(const char *source, size_t &length, char *error, size_t errorSize,
                  size_t capacity) {
    if (!source) {
        writeError(error, errorSize, "script source is null");
        return false;
    }
    length = strnlen(source, capacity);
    if (length == capacity) {
        writeError(error, errorSize, capacity == SCRIPT_MAX ?
                   "script source must be at most 16384 bytes" : "callback source must be at most 17408 bytes");
        return false;
    }
    if (length && static_cast<unsigned char>(source[0]) == 0x1b) {
        writeError(error, errorSize, "bytecode is forbidden; script must be text");
        return false;
    }
    return true;
}

bool run(const char *source, const ScriptEvent *event, const ScriptState *initialState,
         ScriptState *resultState, ScriptPlan *plan, bool syntaxOnly,
         char *error, size_t errorSize, ConfigStore *config = nullptr,
         bool configWriteAllowed = false, bool hookMode = false,
         uint16_t expectedHookMask = 0, uint16_t *discoveredMask = nullptr,
         uint32_t scriptId = 0, ScriptUi *inspectedUi = nullptr,
         const ScriptMeters *meters = nullptr, ScriptActivity *activity = nullptr) {
    writeError(error, errorSize, "");
    size_t length = 0;
    if (!sourceLength(source, length, error, errorSize,
                      hookMode || syntaxOnly ? SCRIPT_STORAGE_MAX : SCRIPT_MAX)) return false;
    Compiler compiler{};
    compiler.meters = meters;
    compiler.discoveredActivity = activity;
    compiler.plan = plan;
    compiler.state = resultState;
    compiler.initialState = initialState;
    compiler.event = event;
    compiler.source = source;
    compiler.length = length;
    compiler.syntaxOnly = syntaxOnly;
    compiler.config = config;
    compiler.configWriteAllowed = configWriteAllowed;
    compiler.hookMode = hookMode;
    compiler.expectedHookMask = expectedHookMask;
    compiler.discoveredMask = discoveredMask;
    compiler.scriptId = scriptId;
    compiler.inspectedUi = inspectedUi;
    if (event) compiler.randomState = event->monotonicMs ^
        (static_cast<uint64_t>(event->channel) << 24) ^
        (static_cast<uint64_t>(event->note) << 8) ^ event->velocity;
    lua_State *lua = lua_newstate(allocate, &compiler);
    if (!lua) {
        writeError(error, errorSize, "Lua memory limit exceeded (1 MiB) or allocation failed");
        return false;
    }
    *static_cast<Compiler **>(lua_getextraspace(lua)) = &compiler;
    lua_sethook(lua, instructionHook, LUA_MASKCOUNT, 100);
    lua_pushcfunction(lua, evaluate);
    int status = lua_pcall(lua, 0, 0, 0);
    if (status != LUA_OK) {
        const char *message = lua_type(lua, -1) == LUA_TSTRING ? lua_tostring(lua, -1) : nullptr;
        writeError(error, errorSize, status == LUA_ERRMEM
            ? "Lua memory limit exceeded (1 MiB) or allocation failed"
            : (message ? message : "Lua script failed"));
    }
    lua_close(lua);
    return status == LUA_OK;
}
}

void scriptUtilityCheck(lua_State *lua, bool write, bool read) {
    auto *compiler = context(lua);
    if (compiler->registering) luaL_error(lua, "utility access belongs inside callbacks");
    if (write && !compiler->configWriteAllowed)
        luaL_error(lua, "utility writes require acknowledgment when saving the script");
    if (read && ++compiler->configReads > 4)
        luaL_error(lua, "configuration read limit exceeded (4)");
}
ConfigStore *scriptUtilityConfig(lua_State *lua) { return context(lua)->config; }
ScriptAction *scriptUtilityAppend(lua_State *lua, ScriptKind kind) { return append(lua, kind); }

bool scriptEvaluate(const char *source, const ScriptEvent &event,
                        ScriptState &state, ScriptPlan &plan,
                        char *error, size_t errorSize, ConfigStore *config, bool configWriteAllowed,
                        bool hookMode, uint16_t expectedHookMask, uint32_t scriptId,
                        const ScriptMeters *meters) {
    plan = {};
    ScriptStateEntry eventValue = event.value;
    memcpy(eventValue.key, event.key, sizeof(eventValue.key));
    if (event.channel > 15 || event.note > 127 || event.velocity > 127 ||
        static_cast<unsigned>(event.kind) >= SCRIPT_EVENT_COUNT ||
        strnlen(event.sound, sizeof(event.sound)) == sizeof(event.sound) ||
        strnlen(event.device, sizeof(event.device)) == sizeof(event.device) ||
        strnlen(event.key, sizeof(event.key)) == sizeof(event.key) ||
        ((event.kind == ScriptEventKind::Ui || event.kind == ScriptEventKind::ConfigChange) && event.hasValue &&
         !ScriptStorage::validSetting(eventValue))) {
        writeError(error, errorSize, "MIDI event values are out of range");
        return false;
    }

    if (!validateState(state)) {
        writeError(error, errorSize, "provided state is invalid");
        return false;
    }
    ScriptState updated{};
    bool success = run(source, &event, &state, &updated, &plan, false, error, errorSize,
                       config, configWriteAllowed, hookMode, expectedHookMask, nullptr, scriptId, nullptr, meters);
    if (!success) {
        plan = {};
        return false;
    }

    state = updated;
    return true;
}

const char *scriptEventName(ScriptEventKind kind) {
    switch (kind) {
    case ScriptEventKind::NoteOn: return "note_on";
    case ScriptEventKind::NoteOff: return "note_off";
    case ScriptEventKind::ControlChange: return "cc";
    case ScriptEventKind::SoundStart: return "sound_start";
    case ScriptEventKind::SoundFinish: return "sound_finish";
    case ScriptEventKind::DeviceConnect: return "device_connect";
    case ScriptEventKind::DeviceDisconnect: return "device_disconnect";
    case ScriptEventKind::Run: return "run";
    case ScriptEventKind::Boot: return "boot";
    case ScriptEventKind::Timer: return "timer";
    case ScriptEventKind::ConfigChange: return "config_change";
    case ScriptEventKind::Ui: return "ui";
    case ScriptEventKind::BluetoothChange: return "bluetooth_change";
    case ScriptEventKind::BluetoothMediaKey: return "bluetooth_media_key";
    case ScriptEventKind::DeviceActive: return "device_active";
    }
    return "unknown";
}

bool scriptValidate(const char *source, char *error, size_t errorSize) {
    return run(source, nullptr, nullptr, nullptr, nullptr, true, error, errorSize);
}

bool scriptInspectHooks(const char *source, uint16_t &mask, char *error, size_t errorSize,
                        ScriptActivity *activity) {
    mask = 0;
    if (activity) *activity = {};
    uint16_t discovered = 0;
    if (!run(source, nullptr, nullptr, nullptr, nullptr, false, error, errorSize,
             nullptr, false, true, 0, &discovered, 0, nullptr, nullptr, activity)) return false;
    mask = discovered;
    return true;
}

bool scriptInspectUi(const char *source, ScriptUi &ui, char *error, size_t errorSize) {
    ui = {};
    ScriptUi inspected{};
    if (!run(source, nullptr, nullptr, nullptr, nullptr, false, error, errorSize,
             nullptr, false, true, 0, nullptr, 0, &inspected)) return false;
    ui = inspected;
    return true;
}

bool scriptUiValueValid(const ScriptUiControl &control, const ScriptStateEntry &entry) {
    ScriptStateEntry value = entry;
    memcpy(value.key, control.key, sizeof(value.key));
    if (!ScriptStorage::validSetting(value)) return false;
    switch (control.kind) {
    case ScriptUiKind::Boolean: return entry.kind == ScriptValueKind::Boolean;
    case ScriptUiKind::String: return entry.kind == ScriptValueKind::String;
    case ScriptUiKind::Number:
        return entry.kind == ScriptValueKind::Number &&
            (!control.hasMin || (std::isfinite(control.min) && entry.numberValue >= control.min)) &&
            (!control.hasMax || (std::isfinite(control.max) && entry.numberValue <= control.max)) &&
            (!control.hasMin || !control.hasMax || control.min <= control.max);
    case ScriptUiKind::Button: return false;
    }
    return false;
}

bool scriptMigrate(ScriptDefinition &script, char *error, size_t errorSize) {
    if (error && errorSize) error[0] = '\0';
    if (script.hookMode) return true;
    if (strnlen(script.script, sizeof(script.script)) >= SCRIPT_MAX ||
        strnlen(script.keys, sizeof(script.keys)) == sizeof(script.keys) ||
        script.eventMask > SCRIPT_LEGACY_EVENTS_MASK) {
        writeError(error, errorSize, "invalid legacy script source or event mask");
        return false;
    }
    std::string source = script.script;
    if (source.empty()) {
        uint8_t report[8];
        if (!scriptChordReport(script.keys, report)) {
            writeError(error, errorSize, "invalid legacy keyboard shortcut");
            return false;
        }
        source = "tap(\"" + std::string(script.keys) + "\")";
    }
    if (!scriptValidate(source.c_str(), error, errorSize)) return false;
    // The initializer keeps generated names out of the original source's scope.
    std::string converted =
        "local legacy = function(...)\n"
        "if event.type == \"run\" then event.type = \"note_on\" end\n" + source + "\nend\n";
    for (unsigned kind = 0; kind < 8; ++kind) {
        if (kind == 7 || (script.eventMask & (1U << kind))) {
            converted += "hook(\"";
            converted += scriptEventName(static_cast<ScriptEventKind>(kind));
            converted += "\", function() legacy() end)\n";
        }
    }
    if (converted.size() >= sizeof(script.script)) {
        writeError(error, errorSize, "converted script exceeds migration storage");
        return false;
    }
    uint16_t mask = 0;
    if (!scriptInspectHooks(converted.c_str(), mask, error, errorSize)) return false;
    memcpy(script.script, converted.c_str(), converted.size() + 1);
    script.keys[0] = '\0';
    script.hookMode = true;
    script.eventMask = mask;
    return true;
}

bool scriptCompile(const char *source, ScriptPlan &plan, char *error, size_t errorSize) {
    ScriptEvent event{0, 0, 127, true};
    ScriptState state{};
    return scriptEvaluate(source, event, state, plan, error, errorSize);
}
