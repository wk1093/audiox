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

} // namespace desktop
