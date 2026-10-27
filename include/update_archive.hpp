#pragma once

#include <stddef.h>
#include <stdint.h>

// AudioX update archive v1. All integers are little-endian.
// Header: magic[8] = "AXUPDATE", u32 version, u32 entry_count.
// Each entry: u32 type, u32 crc32, u64 size, followed by size payload bytes.
// Version 1 requires exactly one config, bootloader, kernel, and program entry.
#define AUDIOX_UPDATE_MAGIC "AXUPDATE"
#define AUDIOX_UPDATE_MAGIC_SIZE 8u
#define AUDIOX_UPDATE_VERSION 1u
#define AUDIOX_UPDATE_HEADER_SIZE 16u
#define AUDIOX_UPDATE_ENTRY_HEADER_SIZE 16u
#define AUDIOX_UPDATE_ENTRY_COUNT 4u
#define AUDIOX_UPDATE_CONFIG_MAX (1024u * 1024u)

enum AudioxUpdateEntryType : uint32_t {
    AUDIOX_UPDATE_CONFIG = 1,
    AUDIOX_UPDATE_BOOTLOADER = 2,
    AUDIOX_UPDATE_KERNEL = 3,
    AUDIOX_UPDATE_PROGRAM = 4,
};

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
