#pragma once
#include <cstddef>
#include <string>

namespace audiox::scenes {
constexpr size_t MAX_SCENES = 32;
constexpr size_t MAX_FILE_SIZE = 128 * 1024;
constexpr const char *SCHEMA_HEADER = "audiox-scenes 1\n";
inline bool validId(const std::string &id) {
    if (id.empty() || id.size() > 48) return false;
    for (unsigned char c : id)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    return true;
}
}
