#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audiox::websocket {

struct Frame {
    uint8_t opcode{};
    const uint8_t *payload{};
    size_t length{};
    size_t consumed{};
};

enum class FrameDecodeResult { Invalid, Incomplete, Complete };

inline uint32_t rotateLeft(uint32_t value, unsigned count) {
    return (value << count) | (value >> (32U - count));
}

inline void sha1(const uint8_t *data, size_t length, uint8_t digest[20]) {
    uint32_t h0 = 0x67452301U, h1 = 0xefcdab89U, h2 = 0x98badcfeU;
    uint32_t h3 = 0x10325476U, h4 = 0xc3d2e1f0U;
    const uint64_t bitLength = static_cast<uint64_t>(length) * 8U;
    const size_t paddedLength = ((length + 9U + 63U) / 64U) * 64U;
    for (size_t block = 0; block < paddedLength; block += 64U) {
        uint8_t bytes[64]{};
        for (size_t i = 0; i < 64U; ++i) {
            const size_t index = block + i;
            if (index < length) bytes[i] = data[index];
            else if (index == length) bytes[i] = 0x80U;
        }
        if (block + 64U == paddedLength) {
            for (unsigned i = 0; i < 8; ++i)
                bytes[56U + i] = static_cast<uint8_t>(bitLength >> ((7U - i) * 8U));
        }
        uint32_t w[80]{};
        for (unsigned i = 0; i < 16; ++i) {
            const unsigned j = i * 4U;
            w[i] = (static_cast<uint32_t>(bytes[j]) << 24) |
                   (static_cast<uint32_t>(bytes[j + 1]) << 16) |
                   (static_cast<uint32_t>(bytes[j + 2]) << 8) | bytes[j + 3];
        }
        for (unsigned i = 16; i < 80; ++i)
            w[i] = rotateLeft(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4;
        for (unsigned i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5a827999U; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ed9eba1U; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdcU; }
            else { f = b ^ c ^ d; k = 0xca62c1d6U; }
            const uint32_t temp = rotateLeft(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotateLeft(b, 30); b = a; a = temp;
        }
        h0 += a; h1 += b; h2 += c; h3 += d; h4 += e;
    }
    const uint32_t words[5] = {h0, h1, h2, h3, h4};
    for (unsigned i = 0; i < 5; ++i)
        for (unsigned j = 0; j < 4; ++j)
            digest[i * 4U + j] = static_cast<uint8_t>(words[i] >> ((3U - j) * 8U));
}

inline bool acceptKey(const char *key, char *out, size_t capacity) {
    if (!key || !out || capacity < 29 || std::strlen(key) != 24) return false;
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t decodedLength = 0;
    unsigned bits = 0, lastValue = 0;
    for (const char *p = key; *p; ++p) {
        if (*p == '=') break;
        const char *found = std::strchr(alphabet, *p);
        if (!found) return false;
        lastValue = static_cast<unsigned>(found - alphabet);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            ++decodedLength;
        }
    }
    if (decodedLength != 16 || key[22] != '=' || key[23] != '=' ||
        (lastValue & 0x0fU) != 0) return false;

    char source[61];
    static constexpr char guid[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const size_t keyLength = std::strlen(key);
    std::memcpy(source, key, keyLength);
    std::memcpy(source + keyLength, guid, sizeof(guid) - 1);
    uint8_t digest[20];
    sha1(reinterpret_cast<const uint8_t *>(source), keyLength + sizeof(guid) - 1, digest);
    size_t used = 0;
    for (size_t i = 0; i < sizeof(digest); i += 3) {
        const uint32_t value = (static_cast<uint32_t>(digest[i]) << 16) |
            (i + 1 < sizeof(digest) ? static_cast<uint32_t>(digest[i + 1]) << 8 : 0U) |
            (i + 2 < sizeof(digest) ? digest[i + 2] : 0U);
        out[used++] = alphabet[(value >> 18) & 63U];
        out[used++] = alphabet[(value >> 12) & 63U];
        out[used++] = i + 1 < sizeof(digest) ? alphabet[(value >> 6) & 63U] : '=';
        out[used++] = i + 2 < sizeof(digest) ? alphabet[value & 63U] : '=';
    }
    out[used] = '\0';
    return used == 28;
}

inline FrameDecodeResult decodeClientFrameResult(const uint8_t *data, size_t size, Frame *frame) {
    if (!data || !frame) return FrameDecodeResult::Invalid;
    if (size < 2) return FrameDecodeResult::Incomplete;
    const uint8_t first = data[0], second = data[1];
    if ((first & 0x70U) || !(first & 0x80U) || !(second & 0x80U))
        return FrameDecodeResult::Invalid;
    size_t header = 2;
    uint64_t length = second & 0x7fU;
    if (length == 126) {
        if (size < 4) return FrameDecodeResult::Incomplete;
        length = (static_cast<uint64_t>(data[2]) << 8) | data[3];
        header = 4;
        if (length < 126) return FrameDecodeResult::Invalid;
    } else if (length == 127) {
        return FrameDecodeResult::Invalid;
    }
    if (length > 1024) return FrameDecodeResult::Invalid;
    if (size < header + 4U + length) return FrameDecodeResult::Incomplete;
    frame->opcode = first & 0x0fU;
    frame->payload = data + header + 4U;
    frame->length = static_cast<size_t>(length);
    frame->consumed = header + 4U + frame->length;
    if ((frame->opcode & 0x8U) && frame->length > 125) return FrameDecodeResult::Invalid;
    return FrameDecodeResult::Complete;
}

inline bool decodeClientFrame(const uint8_t *data, size_t size, Frame *frame) {
    return decodeClientFrameResult(data, size, frame) == FrameDecodeResult::Complete;
}

inline void unmask(uint8_t *payload, size_t length, const uint8_t mask[4]) {
    for (size_t i = 0; i < length; ++i) payload[i] ^= mask[i & 3U];
}

inline bool encodeServerFrame(uint8_t opcode, const void *payload, size_t length,
                              uint8_t *out, size_t capacity, size_t *written) {
    const size_t header = length < 126 ? 2 : 4;
    if (!out || !written || length > 65535 || capacity < header + length) return false;
    out[0] = static_cast<uint8_t>(0x80U | opcode);
    if (length < 126) out[1] = static_cast<uint8_t>(length);
    else {
        out[1] = 126;
        out[2] = static_cast<uint8_t>(length >> 8);
        out[3] = static_cast<uint8_t>(length);
    }
    if (length) std::memcpy(out + header, payload, length);
    *written = header + length;
    return true;
}

} // namespace audiox::websocket
