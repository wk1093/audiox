#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace bt {

constexpr uint16_t AVRCP_PSM = 0x0017;
constexpr uint32_t AVRCP_SDP_RECORD_HANDLE = 0x00010001;
constexpr size_t AVRCP_PACKET_MAX = 512;

inline const char *mediaKeyName(uint8_t operation) {
    switch (operation) {
    case 0x41: return "volume_up";
    case 0x42: return "volume_down";
    case 0x43: return "mute";
    case 0x44: return "play";
    case 0x45: return "stop";
    case 0x46: return "pause";
    case 0x47: return "record";
    case 0x48: return "rewind";
    case 0x49: return "fast_forward";
    case 0x4a: return "eject";
    case 0x4b: return "next";
    case 0x4c: return "previous";
    default: return nullptr;
    }
}

struct MediaKey {
    uint8_t operation{};
    bool pressed{};
};

// Single-packet AVCTP / AVRCP 1.0 target. Unsupported commands are answered,
// not converted into script events; responses never generate events.
inline size_t avrcpResponse(const uint8_t *packet, size_t length, uint8_t *out,
                            size_t capacity, MediaKey &key, bool &hasKey) {
    hasKey = false;
    if (!packet || !out || length < 3 || length > AVRCP_PACKET_MAX ||
        capacity < length || (packet[0] & 0x0e)) return 0;
    std::memcpy(out, packet, length);
    out[0] |= 0x02;
    if (packet[1] != 0x11 || packet[2] != 0x0e) {
        out[0] |= 0x01;
        return 3;
    }
    if ((packet[0] & 1) || length < 6) return 0;
    out[3] = 0x08; // AV/C NOT IMPLEMENTED.
    if (packet[3] == 0x00 && packet[4] == 0x48 && packet[5] == 0x7c) {
        if (length < 8 || length != size_t{8} + packet[7]) {
            out[3] = 0x0a; // REJECTED.
        } else if (packet[7] == 0 && mediaKeyName(packet[6] & 0x7f)) {
            key = {static_cast<uint8_t>(packet[6] & 0x7f), (packet[6] & 0x80) == 0};
            hasKey = true;
            out[3] = 0x09; // ACCEPTED.
        }
    } else if (packet[3] == 0x01 && packet[4] == 0xff && length == 11) {
        if (packet[5] == 0x30) {
            out[3] = 0x0c; // STABLE unit info: panel subunit, no company ID.
            out[6] = 0x07;
            out[7] = 0x48;
            out[8] = out[9] = out[10] = 0xff;
        } else if (packet[5] == 0x31 && packet[6] == 0x07) {
            out[3] = 0x0c;
            out[7] = 0x48;
            out[8] = out[9] = out[10] = 0xff;
        }
    }
    return length;
}

} // namespace bt
