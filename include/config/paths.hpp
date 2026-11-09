#pragma once

#include "config/storage.hpp"
#include "config/schema.hpp"
#include <cstring>
#include <string>

namespace audiox::config_storage {
inline std::string rootfsPath(const char *suffix) {
    std::string normalized;
    const char *part = suffix;
    for (const char *p = suffix;; ++p) {
        if (*p != '/' && *p) continue;
        size_t length = static_cast<size_t>(p - part);
        if (length && !(length == 1 && part[0] == '.')) {
            if (!normalized.empty()) normalized += '/';
            normalized.append(part, length);
        }
        if (!*p) break;
        part = p + 1;
    }
    suffix = normalized.c_str();
    for (const char *name : legacyNames) {
        size_t length = strlen(name);
        if (!strcmp(suffix, name) ||
            (!strncmp(suffix, name, length) && suffix[length] == '/'))
            return std::string("config/") + suffix;
    }
    if (!strcmp(suffix, "scripts.txt")) return "config/scripts.txt";
    return normalized;
}
inline bool rootfsWritable(const char *root, const char *suffix) {
    std::string relative = rootfsPath(suffix);
    if (relative.compare(0, 7, "config/")) return true;
    const std::string name = relative.substr(7);
    if (!name.empty() && name[0] == '.') return false;
    const std::string path = std::string(root) + "/" + relative;
    return configSchemaWritable(checkNamedConfigSchema(path.c_str(), name.c_str()));
}
}
