#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

// Routing thing IDs for paired Bluetooth devices: "bt_<MAC>_in" / "bt_<MAC>_out",
// where <MAC> is the 12 uppercase hex digits of the address in display order
// (for example bt_A4C6F0A57AA7_in). Header-only so the audio engine can use it
// without linking the Bluetooth worker.
namespace bt {

#define BT_ROUTE_PREFIX "bt_"
#define BT_ROUTE_ID_MAX 24

// Packs the little-endian wire address into a non-zero integer (0 means "none").
inline uint64_t packAddress(const uint8_t b[6]) {
    uint64_t value = 0;
    for (int i = 5; i >= 0; --i) {
        value = (value << 8) | b[i];
    }
    return value;
}

inline bool formatRouteId(const uint8_t b[6], bool input, char *out, size_t outSize) {
    if (!out || outSize == 0) {
        return false;
    }
    int n = snprintf(out, outSize, BT_ROUTE_PREFIX "%02X%02X%02X%02X%02X%02X_%s",
                     b[5], b[4], b[3], b[2], b[1], b[0], input ? "in" : "out");
    return n > 0 && (size_t)n < outSize;
}

inline bool formatRouteIdFromPacked(uint64_t packed, bool input, char *out, size_t outSize) {
    uint8_t b[6] = {};
    for (int i = 0; i < 6; ++i) {
        b[i] = (uint8_t)(packed >> (8 * i));
    }
    return formatRouteId(b, input, out, outSize);
}

// Returns the packed address for a Bluetooth route id, or 0 if the id is not one.
inline uint64_t parseRouteId(const char *id, bool *inputOut) {
    const size_t prefixLength = sizeof(BT_ROUTE_PREFIX) - 1U;
    if (!id || strncmp(id, BT_ROUTE_PREFIX, prefixLength) != 0) {
        return 0;
    }
    const char *hex = id + prefixLength;
    uint64_t value = 0;
    for (int i = 0; i < 12; ++i) {
        const char c = hex[i];
        uint8_t nibble = 0;
        if (c >= '0' && c <= '9') nibble = (uint8_t)(c - '0');
        else if (c >= 'A' && c <= 'F') nibble = (uint8_t)(c - 'A' + 10);
        else if (c >= 'a' && c <= 'f') nibble = (uint8_t)(c - 'a' + 10);
        else return 0;
        value = (value << 4) | nibble;
    }
    const char *suffix = hex + 12;
    bool input = false;
    if (strcmp(suffix, "_in") == 0) {
        input = true;
    } else if (strcmp(suffix, "_out") != 0) {
        return 0;
    }
    if (inputOut) {
        *inputOut = input;
    }
    return value;
}

} // namespace bt
