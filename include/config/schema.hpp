#pragma once

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

inline constexpr uint32_t CONFIG_SCHEMA_VERSION = 1;
inline constexpr uint32_t ROUTING_SCHEMA_VERSION = 1;
inline constexpr uint32_t MIDI_MAP_SCHEMA_VERSION = 1;
inline constexpr uint32_t FX_PARAMS_SCHEMA_VERSION = 1;
inline constexpr uint32_t VOLUMES_SCHEMA_VERSION = 1;
inline constexpr uint32_t BLUETOOTH_PAIRED_SCHEMA_VERSION = 1;

enum class ConfigTextSchemaStatus {
    Legacy,
    Supported,
    Unsupported,
    Invalid,
    IoError,
};

inline ConfigTextSchemaStatus checkConfigTextSchema(FILE *file, const char *headerPrefix,
                                                     uint32_t supportedVersion,
                                                     uint32_t *versionOut = nullptr) {
    if (!file || !headerPrefix) return ConfigTextSchemaStatus::IoError;
    fpos_t position;
    if (fgetpos(file, &position) != 0 || fseek(file, 0, SEEK_SET) != 0)
        return ConfigTextSchemaStatus::IoError;

    char line[256]{};
    bool found = false;
    bool invalid = false;
    uint32_t version = 0;
    while (fgets(line, sizeof(line), file)) {
        char *header = line;
        while (*header == ' ' || *header == '\t') ++header;
        if (strncmp(header, headerPrefix, strlen(headerPrefix)) != 0) continue;
        if (found) {
            invalid = true;
            break;
        }
        found = true;
        const char *digits = header + strlen(headerPrefix);
        if (*digits < '0' || *digits > '9') {
            invalid = true;
            break;
        }
        errno = 0;
        char *end = nullptr;
        unsigned long parsed = strtoul(digits, &end, 10);
        if (errno || end == digits || parsed > UINT32_MAX) {
            invalid = true;
            break;
        }
        while (*end == '\r' || *end == '\n' || *end == ' ' || *end == '\t') ++end;
        if (*end) {
            invalid = true;
            break;
        }
        version = static_cast<uint32_t>(parsed);
    }
    bool readError = ferror(file) != 0;
    bool restoreError = fsetpos(file, &position) != 0;
    if (readError || restoreError) return ConfigTextSchemaStatus::IoError;
    if (invalid) return ConfigTextSchemaStatus::Invalid;
    if (!found) {
        if (versionOut) *versionOut = 0;
        return ConfigTextSchemaStatus::Legacy;
    }
    if (versionOut) *versionOut = version;
    return version <= supportedVersion ? ConfigTextSchemaStatus::Supported
                                       : ConfigTextSchemaStatus::Unsupported;
}

inline ConfigTextSchemaStatus checkConfigTextFileSchema(const char *path, const char *headerPrefix,
                                                         uint32_t supportedVersion,
                                                         uint32_t *versionOut = nullptr) {
    if (!path) return ConfigTextSchemaStatus::IoError;
    FILE *file = fopen(path, "r");
    if (!file) {
        if (errno == ENOENT) {
            if (versionOut) *versionOut = 0;
            return ConfigTextSchemaStatus::Legacy;
        }
        return ConfigTextSchemaStatus::IoError;
    }
    ConfigTextSchemaStatus status =
        checkConfigTextSchema(file, headerPrefix, supportedVersion, versionOut);
    if (fclose(file) != 0 && status != ConfigTextSchemaStatus::IoError)
        return ConfigTextSchemaStatus::IoError;
    return status;
}
