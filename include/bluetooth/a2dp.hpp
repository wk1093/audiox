#pragma once

#include <cstddef>
#include <cstdint>

// Stateless A2DP sink helpers: the SDP server and SBC configuration rules.
namespace bt {

// SBC codec information element bits (A2DP 4.3.2).
constexpr uint8_t A2DP_SBC_FREQ_44100 = 0x20;
constexpr uint8_t A2DP_SBC_FREQ_48000 = 0x10;
constexpr uint8_t A2DP_SBC_MIN_BITPOOL = 2;
constexpr uint8_t A2DP_SBC_MAX_BITPOOL = 53;

// SBC capabilities advertised by the AudioX sink endpoint: 44.1/48 kHz, every
// channel mode, block length, subband count, and allocation method.
extern const uint8_t A2DP_SBC_SINK_CAPABILITIES[4];

// Returns the 4-byte SBC element inside an AVDTP service capability list, or
// nullptr when no audio SBC media codec capability is present.
const uint8_t *findSbcCodec(const uint8_t *capabilities, size_t length);

// True when a remote SetConfiguration picks exactly one value for every SBC
// field and that value is one the sink can decode.
bool isValidSbcSinkConfiguration(const uint8_t configuration[4]);

// Chooses a sink configuration from a remote source's SBC capabilities.
// Prefers 48 kHz, joint stereo, 16 blocks, 8 subbands, and loudness.
bool selectSbcSinkConfiguration(const uint8_t remote[4], uint8_t out[4]);

// 44100 or 48000 for a valid sink configuration, 0 otherwise.
uint32_t sbcSampleRate(const uint8_t configuration[4]);

// Handle of the single A2DP Sink service record served by AudioX.
constexpr uint32_t A2DP_SDP_RECORD_HANDLE = 0x00010000U;

// Builds the SDP response PDU for one request PDU. Returns the response
// length, or 0 if the request is too short to answer.
size_t buildSdpResponse(const uint8_t *request, size_t length, uint8_t *out, size_t capacity);

} // namespace bt
