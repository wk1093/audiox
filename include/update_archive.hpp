#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// AudioX update archive. All integers are little-endian.
// Header: magic[8] = "AXUPDATE", u32 version, u32 entry_count.
// Each entry: u32 type, u32 crc32, u64 size, followed by size payload bytes.
// Version 1 requires exactly one config, bootloader, kernel, and program entry; its kernel is
// installed as kernel8.img. Version 2 additionally requires one metadata entry: "key=value" lines
// with at least board= (pi4, pi5) and kernel= (boot partition file name of the kernel entry).
// Optional keys: version=, kernel_version=. Unknown keys are ignored.
#define AUDIOX_UPDATE_MAGIC "AXUPDATE"
#define AUDIOX_UPDATE_MAGIC_SIZE 8u
#define AUDIOX_UPDATE_VERSION_V1 1u
#define AUDIOX_UPDATE_VERSION 2u
#define AUDIOX_UPDATE_HEADER_SIZE 16u
#define AUDIOX_UPDATE_ENTRY_HEADER_SIZE 16u
#define AUDIOX_UPDATE_ENTRY_COUNT_V1 4u
#define AUDIOX_UPDATE_ENTRY_COUNT 5u
#define AUDIOX_UPDATE_CONFIG_MAX (1024u * 1024u)
#define AUDIOX_UPDATE_METADATA_MAX 4096u
#define AUDIOX_UPDATE_VALUE_MAX 64u
#define AUDIOX_UPDATE_DEFAULT_KERNEL "kernel8.img"

enum AudioxUpdateEntryType : uint32_t {
    AUDIOX_UPDATE_CONFIG = 1,
    AUDIOX_UPDATE_BOOTLOADER = 2,
    AUDIOX_UPDATE_KERNEL = 3,
    AUDIOX_UPDATE_PROGRAM = 4,
    AUDIOX_UPDATE_METADATA = 5,
};

struct AudioxUpdateMetadata {
    char board[AUDIOX_UPDATE_VALUE_MAX];
    char kernel[AUDIOX_UPDATE_VALUE_MAX];
    char version[AUDIOX_UPDATE_VALUE_MAX];
    char kernel_version[AUDIOX_UPDATE_VALUE_MAX];
};

// Metadata values are restricted to [A-Za-z0-9._+-] so they are safe as file names and in logs.
static inline bool audioxUpdateValidValue(const char *value, size_t len) {
    if (len == 0 || len >= AUDIOX_UPDATE_VALUE_MAX) {
        return false;
    }
    for (size_t i = 0; i < len; ++i) {
        char c = value[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '+' || c == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

// Kernel names must look like kernel*.img and stay inside the boot partition root.
static inline bool audioxUpdateValidKernelName(const char *name) {
    size_t len = strlen(name);
    return audioxUpdateValidValue(name, len) && len >= 10 && memcmp(name, "kernel", 6) == 0 &&
           memcmp(name + len - 4, ".img", 4) == 0;
}

static inline bool audioxUpdateValidBoard(const char *board) {
    return strcmp(board, "pi4") == 0 || strcmp(board, "pi5") == 0;
}

static inline bool audioxUpdateKeyIs(const char *key, size_t key_len, const char *name) {
    return strlen(name) == key_len && memcmp(key, name, key_len) == 0;
}

// Finds the first "kernel=" line in config.txt text (conditional sections are not evaluated).
// Writes the default kernel name when no valid line is present.
static inline void audioxConfigKernelName(const char *text, size_t len, char *out, size_t out_sz) {
    snprintf(out, out_sz, "%s", AUDIOX_UPDATE_DEFAULT_KERNEL);
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') {
            ++end;
        }
        size_t p = pos;
        while (p < end && (text[p] == ' ' || text[p] == '\t')) {
            ++p;
        }
        if (end - p > 7 && memcmp(text + p, "kernel=", 7) == 0) {
            size_t v = p + 7;
            size_t v_end = v;
            while (v_end < end && text[v_end] != ' ' && text[v_end] != '\t' && text[v_end] != '\r' &&
                   text[v_end] != '#') {
                ++v_end;
            }
            char name[AUDIOX_UPDATE_VALUE_MAX];
            size_t name_len = v_end - v;
            if (name_len < sizeof(name)) {
                memcpy(name, text + v, name_len);
                name[name_len] = '\0';
                if (audioxUpdateValidKernelName(name)) {
                    snprintf(out, out_sz, "%s", name);
                }
            }
            return;
        }
        pos = end + 1;
    }
}

// Parses a metadata payload. Returns NULL on success or a short error description.
static inline const char *audioxUpdateParseMetadata(const char *text, size_t len, AudioxUpdateMetadata *out) {
    out->board[0] = '\0';
    out->kernel[0] = '\0';
    out->version[0] = '\0';
    out->kernel_version[0] = '\0';
    size_t pos = 0;
    while (pos < len) {
        size_t end = pos;
        while (end < len && text[end] != '\n') {
            ++end;
        }
        size_t line_end = end;
        if (line_end > pos && text[line_end - 1] == '\r') {
            --line_end;
        }
        size_t eq = pos;
        while (eq < line_end && text[eq] != '=') {
            ++eq;
        }
        if (line_end > pos && text[pos] != '#') {
            if (eq == line_end) {
                return "malformed metadata line";
            }
            const char *key = text + pos;
            size_t key_len = eq - pos;
            const char *value = text + eq + 1;
            size_t value_len = line_end - eq - 1;
            char *dest = NULL;
            if (audioxUpdateKeyIs(key, key_len, "board")) {
                dest = out->board;
            } else if (audioxUpdateKeyIs(key, key_len, "kernel")) {
                dest = out->kernel;
            } else if (audioxUpdateKeyIs(key, key_len, "version")) {
                dest = out->version;
            } else if (audioxUpdateKeyIs(key, key_len, "kernel_version")) {
                dest = out->kernel_version;
            }
            if (dest) {
                if (!audioxUpdateValidValue(value, value_len)) {
                    return "invalid metadata value";
                }
                for (size_t i = 0; i < value_len; ++i) {
                    dest[i] = value[i];
                }
                dest[value_len] = '\0';
            }
        }
        pos = end + 1;
    }
    if (!out->board[0] || !audioxUpdateValidBoard(out->board)) {
        return "metadata board is missing or unsupported";
    }
    if (!out->kernel[0] || !audioxUpdateValidKernelName(out->kernel)) {
        return "metadata kernel name is missing or invalid";
    }
    return NULL;
}

static inline uint32_t audioxUpdateCrc32(uint32_t crc, const void *data, size_t len) {
    const uint8_t *p = (const uint8_t *)data;
    crc = ~crc;
    for (size_t i = 0; i < len; ++i) {
        crc ^= p[i];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1) ^ (0xedb88320u & (uint32_t)-(int32_t)(crc & 1u));
        }
    }
    return ~crc;
}

static inline void audioxUpdatePutU32(uint8_t *out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16);
    out[3] = (uint8_t)(value >> 24);
}

static inline void audioxUpdatePutU64(uint8_t *out, uint64_t value) {
    for (int i = 0; i < 8; ++i) {
        out[i] = (uint8_t)(value >> (i * 8));
    }
}

static inline uint32_t audioxUpdateGetU32(const uint8_t *in) {
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static inline uint64_t audioxUpdateGetU64(const uint8_t *in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i) {
        value |= (uint64_t)in[i] << (i * 8);
    }
    return value;
}
