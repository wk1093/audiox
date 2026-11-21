#pragma once

#include <initializer_list>
#include <string_view>

namespace desktop {

class JsonCursor {
    std::string_view input;
    size_t pos{};

    void space() {
        while (pos < input.size() && (input[pos] == ' ' || input[pos] == '\t' ||
               input[pos] == '\n' || input[pos] == '\r')) ++pos;
    }

    bool string() {
        if (pos == input.size() || input[pos++] != '"') return false;
        while (pos < input.size()) {
            const unsigned char c = static_cast<unsigned char>(input[pos++]);
            if (c == '"') return true;
            if (c < 0x20) return false;
            if (c != '\\') continue;
            if (pos == input.size()) return false;
            const char escape = input[pos++];
            if (escape == 'u') {
                for (unsigned i = 0; i < 4; ++i) {
                    if (pos == input.size()) return false;
                    const char hex = input[pos++];
                    if (!((hex >= '0' && hex <= '9') || (hex >= 'a' && hex <= 'f') ||
                          (hex >= 'A' && hex <= 'F'))) return false;
                }
            } else if (escape != '"' && escape != '\\' && escape != '/' && escape != 'b' &&
                       escape != 'f' && escape != 'n' && escape != 'r' && escape != 't') return false;
        }
        return false;
    }

    bool value(unsigned depth) {
        if (depth > 64) return false;
        space();
        if (pos == input.size()) return false;
        if (input[pos] == '"') return string();
        if (input[pos] == '{' || input[pos] == '[') {
            const bool object = input[pos++] == '{';
            const char end = object ? '}' : ']';
            space();
            if (pos < input.size() && input[pos] == end) { ++pos; return true; }
            for (;;) {
                space();
                if (object) {
                    if (!string()) return false;
                    space();
                    if (pos == input.size() || input[pos++] != ':') return false;
                }
                if (!value(depth + 1)) return false;
                space();
                if (pos == input.size()) return false;
                if (input[pos] == end) { ++pos; return true; }
                if (input[pos++] != ',') return false;
            }
        }
        for (const std::string_view literal : {"true", "false", "null"}) {
            if (input.substr(pos, literal.size()) == literal) { pos += literal.size(); return true; }
        }
        const size_t start = pos;
        if (input[pos] == '-') ++pos;
        if (pos == input.size()) return false;
        if (input[pos] == '0') ++pos;
        else {
            if (input[pos] < '1' || input[pos] > '9') return false;
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
        }
        if (pos < input.size() && input[pos] == '.') {
            ++pos;
            const size_t digits = pos;
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            if (pos == digits) return false;
        }
        if (pos < input.size() && (input[pos] == 'e' || input[pos] == 'E')) {
            ++pos;
            if (pos < input.size() && (input[pos] == '+' || input[pos] == '-')) ++pos;
            const size_t digits = pos;
            while (pos < input.size() && input[pos] >= '0' && input[pos] <= '9') ++pos;
            if (pos == digits) return false;
        }
        return pos > start;
    }

public:
    explicit JsonCursor(std::string_view source) : input(source) {}

    bool next(std::string_view &out) {
        space();
        const size_t start = pos;
        if (!value(0)) return false;
        out = input.substr(start, pos - start);
        return true;
    }

    bool take(char c) {
        space();
        if (pos == input.size() || input[pos] != c) return false;
        ++pos;
        return true;
    }

    bool finished() { space(); return pos == input.size(); }
};

} // namespace desktop
