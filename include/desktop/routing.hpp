#pragma once

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace desktop {

struct RouteCommand {
    uint32_t id{}, stream{}, sink{};
};

inline bool consume(std::string_view &input, std::string_view prefix) {
    if (input.substr(0, prefix.size()) != prefix) return false;
    input.remove_prefix(prefix.size());
    return true;
}

inline bool number(std::string_view &input, uint32_t &value) {
    if (input.empty() || input.front() < '0' || input.front() > '9') return false;
    const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
    if (parsed.ec != std::errc{}) return false;
    input.remove_prefix(static_cast<size_t>(parsed.ptr - input.data()));
    return true;
}

inline std::string routeCommand(const RouteCommand &command) {
    return "{\"type\":\"route\",\"id\":" + std::to_string(command.id) +
        ",\"stream\":" + std::to_string(command.stream) +
        ",\"sink\":" + std::to_string(command.sink) + "}";
}

inline bool parseRouteCommand(std::string_view input, RouteCommand &command) {
    RouteCommand parsed;
    if (!consume(input, "{\"type\":\"route\",\"id\":") || !number(input, parsed.id) || !parsed.id ||
        !consume(input, ",\"stream\":") || !number(input, parsed.stream) ||
        !consume(input, ",\"sink\":") || !number(input, parsed.sink) ||
        !consume(input, "}") || !input.empty()) return false;
    command = parsed;
    return true;
}

inline std::string routeResult(uint32_t id, bool ok) {
    return "{\"type\":\"route_result\",\"id\":" + std::to_string(id) +
        ",\"ok\":" + (ok ? "true}" : "false}");
}

inline bool parseRouteResult(std::string_view input, uint32_t &id, bool &ok) {
    uint32_t parsedId;
    if (!consume(input, "{\"type\":\"route_result\",\"id\":") ||
        !number(input, parsedId) || !parsedId || !consume(input, ",\"ok\":")) return false;
    bool parsedOk;
    if (consume(input, "true}")) parsedOk = true;
    else if (consume(input, "false}")) parsedOk = false;
    else return false;
    if (!input.empty()) return false;
    id = parsedId;
    ok = parsedOk;
    return true;
}

struct InventoryCommand {
    uint32_t id{}, offset{};
};

inline std::string inventoryCommand(const InventoryCommand &command) {
    return "{\"type\":\"inventory\",\"id\":" + std::to_string(command.id) +
        ",\"offset\":" + std::to_string(command.offset) + "}";
}

inline bool parseInventoryCommand(std::string_view input, InventoryCommand &command) {
    InventoryCommand parsed;
    if (!consume(input, "{\"type\":\"inventory\",\"id\":") || !number(input, parsed.id) || !parsed.id ||
        !consume(input, ",\"offset\":") || !number(input, parsed.offset) ||
        !consume(input, "}") || !input.empty()) return false;
    command = parsed;
    return true;
}

inline std::string encodeChunk(std::string_view type, uint32_t id, uint32_t offset,
                               std::string_view bytes, bool done) {
    constexpr char hex[] = "0123456789abcdef";
    std::string result = "{\"type\":\"" + std::string(type) + "\",\"id\":" + std::to_string(id) +
        ",\"offset\":" + std::to_string(offset) + ",\"data\":\"";
    for (unsigned char c : bytes) {
        result += hex[c >> 4];
        result += hex[c & 15];
    }
    return result + "\",\"done\":" + (done ? "true}" : "false}");
}

inline std::string inventoryChunk(uint32_t id, uint32_t offset, std::string_view bytes, bool done) {
    return encodeChunk("inventory_chunk", id, offset, bytes, done);
}

inline std::string inventoryError(uint32_t id) {
    return "{\"type\":\"inventory_error\",\"id\":" + std::to_string(id) + "}";
}

inline bool parseInventoryError(std::string_view input, uint32_t &id) {
    uint32_t value;
    if (!consume(input, "{\"type\":\"inventory_error\",\"id\":") || !number(input, value) || !value ||
        !consume(input, "}") || !input.empty()) return false;
    id = value;
    return true;
}

inline bool decodeChunk(std::string_view input, std::string_view type, uint32_t &id, uint32_t &offset,
                        std::string &bytes, bool &done) {
    uint32_t parsedId, parsedOffset;
    if (!consume(input, "{\"type\":\"") || !consume(input, type) || !consume(input, "\",\"id\":") ||
        !number(input, parsedId) || !parsedId || !consume(input, ",\"offset\":") ||
        !number(input, parsedOffset) || !consume(input, ",\"data\":\"")) return false;
    const size_t end = input.find('"');
    if (end == std::string_view::npos || end > 800 || end % 2) return false;
    std::string parsed;
    for (size_t i = 0; i < end; i += 2) {
        auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            return -1;
        };
        const int hi = digit(input[i]), lo = digit(input[i + 1]);
        if (hi < 0 || lo < 0) return false;
        parsed += static_cast<char>((hi << 4) | lo);
    }
    input.remove_prefix(end);
    if (!consume(input, "\",\"done\":")) return false;
    bool parsedDone;
    if (consume(input, "true}")) parsedDone = true;
    else if (consume(input, "false}")) parsedDone = false;
    else return false;
    if (!input.empty() || (parsed.empty() && !parsedDone)) return false;
    id = parsedId;
    offset = parsedOffset;
    bytes.swap(parsed);
    done = parsedDone;
    return true;
}

inline bool parseInventoryChunk(std::string_view input, uint32_t &id, uint32_t &offset,
                                std::string &bytes, bool &done) {
    return decodeChunk(input, "inventory_chunk", id, offset, bytes, done);
}

inline bool validSinkName(std::string_view name) {
    if (name.empty() || name.size() > 127 || name.front() == '-' || name.front() == '@') return false;
    bool digits = true;
    for (unsigned char c : name) {
        if (c < 32 || c == 127) return false;
        if (c < '0' || c > '9') digits = false;
    }
    return !digits;
}

inline std::string defaultWatch() { return "{\"type\":\"default_watch\"}"; }
inline std::string defaultSet(uint32_t id, std::string_view name) {
    return encodeChunk("default_set", id, 0, name, true);
}
inline std::string defaultSnapshot(std::string_view name) {
    return encodeChunk("default_snapshot", 1, 0, name, true);
}
inline std::string defaultResult(uint32_t id, std::string_view name, bool ok) {
    return encodeChunk(ok ? "default_result" : "default_failed", id, 0, name, true);
}
inline bool parseDefaultMessage(std::string_view input, std::string_view type,
                                uint32_t &id, std::string &name, bool &done) {
    uint32_t offset;
    if (!decodeChunk(input, type, id, offset, name, done) || offset ||
        (!name.empty() && !validSinkName(name))) return false;
    if (type == "default_set") return done && !name.empty();
    if (type == "default_snapshot") return id == 1 && done;
    if (type == "default_result") return done && !name.empty();
    return type == "default_failed" && done;
}

inline std::string rulesAck(uint32_t id, uint32_t offset, bool ok) {
    return "{\"type\":\"rules_ack\",\"id\":" + std::to_string(id) +
        ",\"offset\":" + std::to_string(offset) + ",\"ok\":" + (ok ? "true}" : "false}");
}

inline bool parseRulesAck(std::string_view input, uint32_t &id, uint32_t &offset, bool &ok) {
    uint32_t parsedId, parsedOffset;
    if (!consume(input, "{\"type\":\"rules_ack\",\"id\":") || !number(input, parsedId) || !parsedId ||
        !consume(input, ",\"offset\":") || !number(input, parsedOffset) || !consume(input, ",\"ok\":"))
        return false;
    bool parsedOk;
    if (consume(input, "true}")) parsedOk = true;
    else if (consume(input, "false}")) parsedOk = false;
    else return false;
    if (!input.empty()) return false;
    id = parsedId;
    offset = parsedOffset;
    ok = parsedOk;
    return true;
}

inline std::string autoStatus(bool ok) {
    return std::string("{\"type\":\"auto_status\",\"ok\":") + (ok ? "true}" : "false}");
}

inline bool parseAutoStatus(std::string_view input, bool &ok) {
    if (input == "{\"type\":\"auto_status\",\"ok\":true}") { ok = true; return true; }
    if (input == "{\"type\":\"auto_status\",\"ok\":false}") { ok = false; return true; }
    return false;
}

} // namespace desktop
