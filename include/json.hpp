#pragma once

#include <string>

namespace audiox::json {

inline std::string escape(const char *text) {
    std::string result;
    for (const unsigned char ch : std::string(text)) {
        if (ch == '"' || ch == '\\') {
            result += '\\';
            result += static_cast<char>(ch);
        } else if (ch < 0x20) {
            constexpr char hex[] = "0123456789abcdef";
            result += "\\u00";
            result += hex[ch >> 4];
            result += hex[ch & 15];
        } else {
            result += static_cast<char>(ch);
        }
    }
    return result;
}

} // namespace audiox::json
