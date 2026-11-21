#pragma once

#include "routing.hpp"
#include "../../include/desktop/json.hpp"

#include <string>
#include <string_view>

namespace desktop {

inline bool jsonMember(std::string_view object, std::string_view key, std::string_view &out) {
    JsonCursor cursor(object);
    if (!cursor.take('{')) return false;
    if (cursor.take('}')) return false;
    do {
        std::string_view name, value;
        if (!cursor.next(name) || !cursor.take(':') || !cursor.next(value)) return false;
        if (name.size() == key.size() + 2 && name.front() == '"' &&
            name.substr(1, key.size()) == key && name.back() == '"') {
            out = value;
            return true;
        }
        if (cursor.take('}')) return false;
    } while (cursor.take(','));
    return false;
}

inline bool jsonString(std::string_view value, std::string &output) {
    JsonCursor check(value);
    std::string_view parsed;
    if (!check.next(parsed) || !check.finished() || parsed.size() < 2 || parsed.front() != '"') return false;
    std::string result;
    auto codepoint = [&](size_t &pos, uint32_t &code) {
        if (pos + 4 > value.size() - 1) return false;
        code = 0;
        for (unsigned n = 0; n < 4; ++n) {
            const char c = value[pos++];
            unsigned digit;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10;
            else return false;
            code = (code << 4) | digit;
        }
        return true;
    };
    for (size_t i = 1; i < value.size() - 1; ++i) {
        char c = value[i];
        if (c != '\\') { result += c; continue; }
        c = value[++i];
        if (c == '"' || c == '\\' || c == '/') result += c;
        else if (c == 'b') result += '\b';
        else if (c == 'f') result += '\f';
        else if (c == 'n') result += '\n';
        else if (c == 'r') result += '\r';
        else if (c == 't') result += '\t';
        else if (c == 'u') {
            uint32_t code;
            ++i;
            if (!codepoint(i, code)) return false;
            if (code >= 0xd800 && code <= 0xdbff) {
                if (i + 2 >= value.size() || value[i] != '\\' || value[i + 1] != 'u') return false;
                i += 2;
                uint32_t low;
                if (!codepoint(i, low) || low < 0xdc00 || low > 0xdfff) return false;
                code = 0x10000 + ((code - 0xd800) << 10) + low - 0xdc00;
            } else if (code >= 0xdc00 && code <= 0xdfff) return false;
            if (!code) return false;
            if (code < 0x80) result += static_cast<char>(code);
            else if (code < 0x800) {
                result += static_cast<char>(0xc0 | (code >> 6));
                result += static_cast<char>(0x80 | (code & 63));
            } else if (code < 0x10000) {
                result += static_cast<char>(0xe0 | (code >> 12));
                result += static_cast<char>(0x80 | ((code >> 6) & 63));
                result += static_cast<char>(0x80 | (code & 63));
            } else {
                result += static_cast<char>(0xf0 | (code >> 18));
                result += static_cast<char>(0x80 | ((code >> 12) & 63));
                result += static_cast<char>(0x80 | ((code >> 6) & 63));
                result += static_cast<char>(0x80 | (code & 63));
            }
            --i;
        } else return false;
    }
    output.swap(result);
    return true;
}

inline bool nodeInventory(std::string_view dump, std::string &output) {
    JsonCursor cursor(dump);
    if (!cursor.take('[')) return false;
    std::string result = "[";
    size_t count = 0;
    if (!cursor.take(']')) {
        for (;;) {
            std::string_view node, type, id, info, props;
            if (!cursor.next(node)) return false;
            if (jsonMember(node, "type", type) && type == "\"PipeWire:Interface:Node\"") {
                unsigned parsed;
                if (!jsonMember(node, "id", id) || !streamNumber(id, parsed) || ++count > 2048) return false;
                if (jsonMember(node, "info", info)) (void)jsonMember(info, "props", props);
                if (count > 1) result += ',';
                result += "{\"id\":";
                result.append(id.data(), id.size());
                for (const auto key : {"node.name", "node.description", "node.nick",
                                       "application.name", "application.id", "media.class",
                                       "object.serial", "audio.position", "audio.channels"}) {
                    std::string_view value;
                    if (!jsonMember(props, key, value) || value == "null") continue;
                    if (value.empty() || value.size() > 4096) return false;
                    const bool serial = std::string_view(key) == "object.serial";
                    const bool channels = std::string_view(key) == "audio.channels";
                    if (serial) {
                        if (value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
                        if (value.empty() || value.size() > 20) return false;
                        uint64_t parsedSerial;
                        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), parsedSerial);
                        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return false;
                    } else if (channels) {
                        unsigned count;
                        if (!streamNumber(value, count) || count > 64) return false;
                    } else if (value.front() != '"') return false;
                    result += ",\"";
                    result += key;
                    result += "\":";
                    if (serial) result += '"';
                    result.append(value.data(), value.size());
                    if (serial) result += '"';
                }
                std::string_view audioChannels, params, formats, format;
                if (!jsonMember(props, "audio.channels", audioChannels) &&
                    jsonMember(info, "params", params) && jsonMember(params, "EnumFormat", formats)) {
                    JsonCursor formatCursor(formats);
                    if (formatCursor.take('[') && formatCursor.next(format) &&
                        jsonMember(format, "channels", audioChannels)) {
                        unsigned channels;
                        if (streamNumber(audioChannels, channels) && channels <= 64)
                            result += ",\"audio.channels\":" + std::to_string(channels);
                    }
                }
                result += '}';
                if (result.size() > 262144) return false;
            }
            if (cursor.take(']')) break;
            if (!cursor.take(',')) return false;
        }
    }
    if (!cursor.finished()) return false;
    result += ']';
    output.swap(result);
    return true;
}

inline bool discoverNodes(int connection, std::string &output) {
    std::string dump;
    if (!command("pw-dump", {"--no-colors"}, connection, dump, 8 * 1024 * 1024)) {
        std::fprintf(stderr, "AudioX: PipeWire discovery command failed or exceeded its limit.\n");
        return false;
    }
    if (!nodeInventory(dump, output)) {
        std::fprintf(stderr, "AudioX: invalid or oversized PipeWire discovery output.\n");
        return false;
    }
    return true;
}

} // namespace desktop
