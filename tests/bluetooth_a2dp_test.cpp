#include "bluetooth/a2dp.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>

static std::vector<uint8_t> sdp(uint8_t pdu, const std::vector<uint8_t> &params) {
    std::vector<uint8_t> request = {pdu, 0x12, 0x34,
                                    (uint8_t)(params.size() >> 8), (uint8_t)params.size()};
    request.insert(request.end(), params.begin(), params.end());
    uint8_t out[512] = {};
    const size_t length = bt::buildSdpResponse(request.data(), request.size(), out, sizeof(out));
    assert(length >= 5);
    assert(out[1] == 0x12 && out[2] == 0x34);
    assert((size_t)((out[3] << 8) | out[4]) == length - 5);
    return std::vector<uint8_t>(out, out + length);
}

static bool contains(const std::vector<uint8_t> &haystack, const std::vector<uint8_t> &needle) {
    for (size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (memcmp(haystack.data() + i, needle.data(), needle.size()) == 0) {
            return true;
        }
    }
    return false;
}

static void checkSdp() {
    const std::vector<uint8_t> allAttributes = {0x35, 0x05, 0x0A, 0x00, 0x00, 0xFF, 0xFF};
    const std::vector<uint8_t> audioSinkUuid = {0x35, 0x03, 0x19, 0x11, 0x0B};

    // Service search: Audio Sink matches, Audio Source does not.
    auto response = sdp(0x02, {0x35, 0x03, 0x19, 0x11, 0x0B, 0x00, 0x10, 0x00});
    assert(response[0] == 0x03);
    assert(response[5] == 0 && response[6] == 1 && response[7] == 0 && response[8] == 1);
    assert(contains(response, {0x00, 0x01, 0x00, 0x00}));
    response = sdp(0x02, {0x35, 0x03, 0x19, 0x11, 0x0A, 0x00, 0x10, 0x00});
    assert(response[0] == 0x03 && response[6] == 0 && response[8] == 0);

    // 128-bit UUID of A2DP on the Bluetooth base UUID.
    std::vector<uint8_t> uuid128 = {0x35, 0x11, 0x1C, 0x00, 0x00, 0x11, 0x0D, 0x00, 0x00, 0x10, 0x00,
                                    0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
    uuid128.insert(uuid128.end(), {0x00, 0x10, 0x00});
    response = sdp(0x02, uuid128);
    assert(response[6] == 1);

    // Search attribute returns a record with the handle, Audio Sink class, and AVDTP PSM.
    std::vector<uint8_t> params = audioSinkUuid;
    params.insert(params.end(), {0x01, 0x00});
    params.insert(params.end(), allAttributes.begin(), allAttributes.end());
    params.push_back(0x00);
    response = sdp(0x06, params);
    assert(response[0] == 0x07);
    assert(contains(response, {0x09, 0x00, 0x00, 0x0A, 0x00, 0x01, 0x00, 0x00}));
    assert(contains(response, {0x09, 0x00, 0x01, 0x35, 0x03, 0x19, 0x11, 0x0B}));
    assert(contains(response, {0x19, 0x01, 0x00, 0x09, 0x00, 0x19}));
    assert(response.back() == 0x00);

    // Non-matching search attribute returns an empty outer sequence.
    params = {0x35, 0x03, 0x19, 0x11, 0x0A, 0x01, 0x00};
    params.insert(params.end(), allAttributes.begin(), allAttributes.end());
    params.push_back(0x00);
    response = sdp(0x06, params);
    assert(response[0] == 0x07 && response[5] == 0 && response[6] == 2);
    assert(response[7] == 0x35 && response[8] == 0x00);

    // Attribute filtering returns only requested IDs.
    response = sdp(0x04, {0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x35, 0x03, 0x09, 0x00, 0x09, 0x00});
    assert(response[0] == 0x05);
    assert(contains(response, {0x09, 0x00, 0x09, 0x35, 0x08}));
    assert(!contains(response, {0x09, 0x00, 0x01, 0x35}));

    // Unknown handle is an error.
    response = sdp(0x04, {0x00, 0x02, 0x00, 0x00, 0x01, 0x00, 0x35, 0x03, 0x09, 0x00, 0x09, 0x00});
    assert(response[0] == 0x01 && response[6] == 0x02);

    // Small byte budgets are served with continuation state.
    std::vector<uint8_t> collected;
    uint16_t resume = 0;
    for (int round = 0; round < 64; ++round) {
        params = audioSinkUuid;
        params.insert(params.end(), {0x00, 0x10});
        params.insert(params.end(), allAttributes.begin(), allAttributes.end());
        if (resume) {
            params.insert(params.end(), {0x02, (uint8_t)(resume >> 8), (uint8_t)resume});
        } else {
            params.push_back(0x00);
        }
        response = sdp(0x06, params);
        assert(response[0] == 0x07);
        const size_t chunk = (size_t)((response[5] << 8) | response[6]);
        assert(chunk <= 0x10);
        collected.insert(collected.end(), response.begin() + 7, response.begin() + 7 + (long)chunk);
        const uint8_t continuation = response[7 + chunk];
        if (continuation == 0) {
            break;
        }
        assert(continuation == 2);
        resume = (uint16_t)((response[8 + chunk] << 8) | response[9 + chunk]);
    }
    params = audioSinkUuid;
    params.insert(params.end(), {0x01, 0x00});
    params.insert(params.end(), allAttributes.begin(), allAttributes.end());
    params.push_back(0x00);
    response = sdp(0x06, params);
    const size_t whole = (size_t)((response[5] << 8) | response[6]);
    assert(collected.size() == whole);
    assert(memcmp(collected.data(), response.data() + 7, whole) == 0);

    // Malformed input gets an SDP error, not silence.
    response = sdp(0x06, {0x35, 0x00, 0x01, 0x00});
    assert(response[0] == 0x01 && response[6] == 0x03);
    response = sdp(0x08, {});
    assert(response[0] == 0x01);
}

static void checkSbc() {
    const uint8_t capabilities[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00, 0x21, 0x15, 0x02, 0x35};
    const uint8_t *sbc = bt::findSbcCodec(capabilities, sizeof(capabilities));
    assert(sbc == capabilities + 6);
    const uint8_t aac[] = {0x07, 0x08, 0x00, 0x02, 0, 0, 0, 0, 0, 0};
    assert(bt::findSbcCodec(aac, sizeof(aac)) == nullptr);
    const uint8_t truncated[] = {0x07, 0x06, 0x00, 0x00, 0x21};
    assert(bt::findSbcCodec(truncated, sizeof(truncated)) == nullptr);

    // Typical Android SBC source: 44.1 kHz only, mono or joint stereo, loudness.
    const uint8_t android[] = {0x29, 0xFC | 0x04 | 0x01, 0x02, 0x35};
    uint8_t selected[4] = {};
    assert(bt::selectSbcSinkConfiguration(android, selected));
    assert(selected[0] == 0x21 && selected[1] == 0x15 && selected[2] == 2 && selected[3] == 53);
    assert(bt::isValidSbcSinkConfiguration(selected));
    assert(bt::sbcSampleRate(selected) == 44100);

    const uint8_t full[] = {0xFF, 0xFF, 0x0A, 0xFA};
    assert(bt::selectSbcSinkConfiguration(full, selected));
    assert(selected[0] == 0x11 && selected[2] == 10 && selected[3] == 53);
    assert(bt::sbcSampleRate(selected) == 48000);

    const uint8_t only32k[] = {0x41, 0xFF, 0x02, 0x35};
    assert(!bt::selectSbcSinkConfiguration(only32k, selected));
    const uint8_t badBitpool[] = {0x11, 0xFF, 0x40, 0x50};
    assert(!bt::selectSbcSinkConfiguration(badBitpool, selected));

    const uint8_t stereo8Blocks[] = {0x12, 0x46, 0x02, 0x30};
    assert(bt::isValidSbcSinkConfiguration(stereo8Blocks));
    const uint8_t twoModes[] = {0x13, 0x15, 0x02, 0x35};
    assert(!bt::isValidSbcSinkConfiguration(twoModes));
    const uint8_t at32k[] = {0x41, 0x15, 0x02, 0x35};
    assert(!bt::isValidSbcSinkConfiguration(at32k));
    const uint8_t minAboveMax[] = {0x11, 0x15, 0x30, 0x20};
    assert(!bt::isValidSbcSinkConfiguration(minAboveMax));
    assert(bt::A2DP_SBC_SINK_CAPABILITIES[0] == 0x3F);
}

int main() {
    checkSdp();
    checkSbc();
    printf("bluetooth a2dp tests passed\n");
    return 0;
}
