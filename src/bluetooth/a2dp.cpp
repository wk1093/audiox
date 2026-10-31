#include "bluetooth/a2dp.hpp"

#include <string.h>

namespace bt {

const uint8_t A2DP_SBC_SINK_CAPABILITIES[4] = {
    A2DP_SBC_FREQ_44100 | A2DP_SBC_FREQ_48000 | 0x0F,
    0xFF,
    A2DP_SBC_MIN_BITPOOL,
    A2DP_SBC_MAX_BITPOOL,
};

namespace {

bool singleBit(uint8_t value) {
    return value != 0 && (value & (uint8_t)(value - 1U)) == 0;
}

uint8_t pickFirst(uint8_t available, const uint8_t *preferred, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (available & preferred[i]) {
            return preferred[i];
        }
    }
    return 0;
}

// SDP (Core Vol 3 Part B) ----------------------------------------------------

constexpr uint8_t SDP_ERROR_RESPONSE = 0x01;
constexpr uint8_t SDP_SEARCH_REQUEST = 0x02;
constexpr uint8_t SDP_ATTRIBUTE_REQUEST = 0x04;
constexpr uint8_t SDP_SEARCH_ATTRIBUTE_REQUEST = 0x06;
constexpr uint16_t SDP_INVALID_HANDLE = 0x0002;
constexpr uint16_t SDP_INVALID_SYNTAX = 0x0003;
constexpr uint16_t SDP_INVALID_PDU_SIZE = 0x0004;
constexpr uint16_t SDP_INVALID_CONTINUATION = 0x0005;
constexpr size_t SDP_MAX_RANGES = 32;
constexpr size_t SDP_MAX_PATTERN = 12;

struct Attribute {
    uint16_t id;
    const uint8_t *value;
    size_t length;
};

const uint8_t kHandle[] = {0x0A, 0x00, 0x01, 0x00, 0x00};
const uint8_t kServiceClasses[] = {0x35, 0x03, 0x19, 0x11, 0x0B};
const uint8_t kProtocols[] = {
    0x35, 0x10,
    0x35, 0x06, 0x19, 0x01, 0x00, 0x09, 0x00, 0x19,
    0x35, 0x06, 0x19, 0x00, 0x19, 0x09, 0x01, 0x03,
};
const uint8_t kBrowseGroups[] = {0x35, 0x03, 0x19, 0x10, 0x02};
const uint8_t kProfiles[] = {0x35, 0x08, 0x35, 0x06, 0x19, 0x11, 0x0D, 0x09, 0x01, 0x03};
const uint8_t kServiceName[] = {0x25, 0x06, 'A', 'u', 'd', 'i', 'o', 'X'};
const uint8_t kSupportedFeatures[] = {0x09, 0x00, 0x02}; // Speaker

// Sorted by attribute ID, as SDP requires.
const Attribute kRecord[] = {
    {0x0000, kHandle, sizeof(kHandle)},
    {0x0001, kServiceClasses, sizeof(kServiceClasses)},
    {0x0004, kProtocols, sizeof(kProtocols)},
    {0x0005, kBrowseGroups, sizeof(kBrowseGroups)},
    {0x0009, kProfiles, sizeof(kProfiles)},
    {0x0100, kServiceName, sizeof(kServiceName)},
    {0x0311, kSupportedFeatures, sizeof(kSupportedFeatures)},
};

// AudioSink, AdvancedAudioDistribution, L2CAP, AVDTP, PublicBrowseRoot.
const uint32_t kRecordUuids[] = {0x110B, 0x110D, 0x0100, 0x0019, 0x1002};

struct Element {
    uint8_t type;
    const uint8_t *data;
    size_t length;
    size_t total;
};

bool readElement(const uint8_t *p, size_t available, Element *element) {
    if (available < 1) {
        return false;
    }
    const uint8_t type = p[0] >> 3;
    const uint8_t sizeIndex = p[0] & 0x07;
    size_t header = 1;
    size_t length = 0;
    if (type != 0) {
        static const size_t fixed[] = {1, 2, 4, 8, 16};
        if (sizeIndex < 5) {
            length = fixed[sizeIndex];
        } else {
            const size_t bytes = sizeIndex == 5 ? 1U : sizeIndex == 6 ? 2U : 4U;
            if (available < 1 + bytes) {
                return false;
            }
            for (size_t i = 0; i < bytes; ++i) {
                length = (length << 8) | p[1 + i];
            }
            header += bytes;
        }
    }
    if (length > available - header) {
        return false;
    }
    element->type = type;
    element->data = p + header;
    element->length = length;
    element->total = header + length;
    return true;
}

// Returns false for malformed patterns; *matches says whether the record has
// every UUID in the pattern.
bool matchPattern(const Element &pattern, bool *matches) {
    if (pattern.type != 6) {
        return false;
    }
    size_t count = 0;
    bool all = true;
    for (size_t offset = 0; offset < pattern.length;) {
        Element uuid = {};
        if (!readElement(pattern.data + offset, pattern.length - offset, &uuid) || uuid.type != 3) {
            return false;
        }
        offset += uuid.total;
        if (++count > SDP_MAX_PATTERN) {
            return false;
        }
        uint32_t value = 0xFFFFFFFFU;
        if (uuid.length == 2 || uuid.length == 4) {
            value = 0;
            for (size_t i = 0; i < uuid.length; ++i) {
                value = (value << 8) | uuid.data[i];
            }
        } else if (uuid.length == 16) {
            static const uint8_t base[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                             0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB};
            if (memcmp(uuid.data + 4, base, sizeof(base)) == 0) {
                value = ((uint32_t)uuid.data[0] << 24) | ((uint32_t)uuid.data[1] << 16) |
                        ((uint32_t)uuid.data[2] << 8) | uuid.data[3];
            }
        } else {
            return false;
        }
        bool found = false;
        for (uint32_t known : kRecordUuids) {
            found = found || known == value;
        }
        all = all && found;
    }
    if (count == 0) {
        return false;
    }
    *matches = all;
    return true;
}

struct Range {
    uint16_t low;
    uint16_t high;
};

bool parseAttributeList(const Element &list, Range *ranges, size_t *count) {
    if (list.type != 6) {
        return false;
    }
    *count = 0;
    for (size_t offset = 0; offset < list.length;) {
        Element item = {};
        if (!readElement(list.data + offset, list.length - offset, &item) || item.type != 1 ||
            *count >= SDP_MAX_RANGES) {
            return false;
        }
        offset += item.total;
        if (item.length == 2) {
            const uint16_t id = (uint16_t)((item.data[0] << 8) | item.data[1]);
            ranges[(*count)++] = {id, id};
        } else if (item.length == 4) {
            ranges[(*count)++] = {(uint16_t)((item.data[0] << 8) | item.data[1]),
                                  (uint16_t)((item.data[2] << 8) | item.data[3])};
        } else {
            return false;
        }
    }
    return *count > 0;
}

bool appendRaw(uint8_t *out, size_t capacity, size_t *length, const uint8_t *data, size_t size) {
    if (size > capacity - *length) {
        return false;
    }
    memcpy(out + *length, data, size);
    *length += size;
    return true;
}

bool appendSequenceHeader(uint8_t *out, size_t capacity, size_t *length, size_t contentLength) {
    if (contentLength <= 0xFF) {
        const uint8_t header[] = {0x35, (uint8_t)contentLength};
        return appendRaw(out, capacity, length, header, sizeof(header));
    }
    const uint8_t header[] = {0x36, (uint8_t)(contentLength >> 8), (uint8_t)contentLength};
    return contentLength <= 0xFFFF && appendRaw(out, capacity, length, header, sizeof(header));
}

// Writes the record's attribute list (a sequence of ID/value pairs) filtered
// by the requested ranges.
bool appendAttributeList(const Range *ranges, size_t rangeCount,
                         uint8_t *out, size_t capacity, size_t *length) {
    uint8_t body[256] = {};
    size_t bodyLength = 0;
    for (const Attribute &attribute : kRecord) {
        bool wanted = false;
        for (size_t i = 0; i < rangeCount; ++i) {
            wanted = wanted || (attribute.id >= ranges[i].low && attribute.id <= ranges[i].high);
        }
        if (!wanted) {
            continue;
        }
        const uint8_t id[] = {0x09, (uint8_t)(attribute.id >> 8), (uint8_t)attribute.id};
        if (!appendRaw(body, sizeof(body), &bodyLength, id, sizeof(id)) ||
            !appendRaw(body, sizeof(body), &bodyLength, attribute.value, attribute.length)) {
            return false;
        }
    }
    return appendSequenceHeader(out, capacity, length, bodyLength) &&
           appendRaw(out, capacity, length, body, bodyLength);
}

void writeBe16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

size_t finishPdu(uint8_t *out, uint8_t pdu, uint16_t transaction, size_t parameterLength) {
    out[0] = pdu;
    writeBe16(out + 1, transaction);
    writeBe16(out + 3, (uint16_t)parameterLength);
    return parameterLength + 5U;
}

size_t errorPdu(uint8_t *out, size_t capacity, uint16_t transaction, uint16_t code) {
    if (capacity < 7) {
        return 0;
    }
    writeBe16(out + 5, code);
    return finishPdu(out, SDP_ERROR_RESPONSE, transaction, 2);
}

} // namespace

const uint8_t *findSbcCodec(const uint8_t *capabilities, size_t length) {
    if (!capabilities) {
        return nullptr;
    }
    for (size_t offset = 0; offset + 2 <= length;) {
        const uint8_t category = capabilities[offset];
        const uint8_t categoryLength = capabilities[offset + 1];
        offset += 2;
        if (categoryLength > length - offset) {
            return nullptr;
        }
        if (category == 0x07 && categoryLength >= 6 &&
            (capabilities[offset] >> 4) == 0 && capabilities[offset + 1] == 0x00) {
            return capabilities + offset + 2;
        }
        offset += categoryLength;
    }
    return nullptr;
}

bool isValidSbcSinkConfiguration(const uint8_t configuration[4]) {
    if (!configuration) {
        return false;
    }
    const uint8_t frequency = configuration[0] & 0xF0;
    return (frequency == A2DP_SBC_FREQ_44100 || frequency == A2DP_SBC_FREQ_48000) &&
           singleBit(configuration[0] & 0x0F) &&
           singleBit(configuration[1] & 0xF0) &&
           singleBit(configuration[1] & 0x0C) &&
           singleBit(configuration[1] & 0x03) &&
           configuration[2] >= A2DP_SBC_MIN_BITPOOL &&
           configuration[2] <= configuration[3] &&
           configuration[3] <= 250;
}

bool selectSbcSinkConfiguration(const uint8_t remote[4], uint8_t out[4]) {
    if (!remote || !out) {
        return false;
    }
    static const uint8_t frequencies[] = {A2DP_SBC_FREQ_48000, A2DP_SBC_FREQ_44100};
    static const uint8_t modes[] = {0x01, 0x02, 0x04, 0x08};
    static const uint8_t blocks[] = {0x10, 0x20, 0x40, 0x80};
    static const uint8_t subbands[] = {0x04, 0x08};
    static const uint8_t allocations[] = {0x01, 0x02};
    const uint8_t frequency = pickFirst(remote[0], frequencies, sizeof(frequencies));
    const uint8_t mode = pickFirst(remote[0], modes, sizeof(modes));
    const uint8_t block = pickFirst(remote[1], blocks, sizeof(blocks));
    const uint8_t subband = pickFirst(remote[1], subbands, sizeof(subbands));
    const uint8_t allocation = pickFirst(remote[1], allocations, sizeof(allocations));
    if (!frequency || !mode || !block || !subband || !allocation) {
        return false;
    }
    const uint8_t minBitpool = remote[2] > A2DP_SBC_MIN_BITPOOL ? remote[2] : A2DP_SBC_MIN_BITPOOL;
    const uint8_t maxBitpool = remote[3] < A2DP_SBC_MAX_BITPOOL ? remote[3] : A2DP_SBC_MAX_BITPOOL;
    if (minBitpool > maxBitpool) {
        return false;
    }
    out[0] = (uint8_t)(frequency | mode);
    out[1] = (uint8_t)(block | subband | allocation);
    out[2] = minBitpool;
    out[3] = maxBitpool;
    return true;
}

uint32_t sbcSampleRate(const uint8_t configuration[4]) {
    if (!configuration) {
        return 0;
    }
    switch (configuration[0] & 0xF0) {
    case A2DP_SBC_FREQ_48000:
        return 48000;
    case A2DP_SBC_FREQ_44100:
        return 44100;
    default:
        return 0;
    }
}

size_t buildSdpResponse(const uint8_t *request, size_t length, uint8_t *out, size_t capacity) {
    if (!request || !out || length < 5 || capacity < 16) {
        return 0;
    }
    const uint8_t pdu = request[0];
    const uint16_t transaction = (uint16_t)((request[1] << 8) | request[2]);
    const size_t parameterLength = (size_t)((request[3] << 8) | request[4]);
    if (parameterLength != length - 5U) {
        return errorPdu(out, capacity, transaction, SDP_INVALID_PDU_SIZE);
    }
    const uint8_t *params = request + 5;
    size_t offset = 0;

    bool matches = true;
    if (pdu == SDP_SEARCH_REQUEST || pdu == SDP_SEARCH_ATTRIBUTE_REQUEST) {
        Element pattern = {};
        if (!readElement(params, parameterLength, &pattern) || !matchPattern(pattern, &matches)) {
            return errorPdu(out, capacity, transaction, SDP_INVALID_SYNTAX);
        }
        offset += pattern.total;
    } else if (pdu == SDP_ATTRIBUTE_REQUEST) {
        if (parameterLength < 4) {
            return errorPdu(out, capacity, transaction, SDP_INVALID_SYNTAX);
        }
        const uint32_t handle = ((uint32_t)params[0] << 24) | ((uint32_t)params[1] << 16) |
                                ((uint32_t)params[2] << 8) | params[3];
        if (handle != A2DP_SDP_RECORD_HANDLE) {
            return errorPdu(out, capacity, transaction, SDP_INVALID_HANDLE);
        }
        offset += 4;
    } else {
        return errorPdu(out, capacity, transaction, SDP_INVALID_SYNTAX);
    }

    if (parameterLength - offset < 2) {
        return errorPdu(out, capacity, transaction, SDP_INVALID_SYNTAX);
    }
    const uint16_t maximum = (uint16_t)((params[offset] << 8) | params[offset + 1]);
    offset += 2;

    if (pdu == SDP_SEARCH_REQUEST) {
        if (parameterLength - offset < 1 || params[offset] != 0) {
            return errorPdu(out, capacity, transaction, SDP_INVALID_CONTINUATION);
        }
        const uint16_t total = matches ? 1 : 0;
        const uint16_t current = total < maximum ? total : maximum;
        if (capacity < 5U + 5U + 4U * current) {
            return 0;
        }
        uint8_t *response = out + 5;
        writeBe16(response, total);
        writeBe16(response + 2, current);
        size_t responseLength = 4;
        if (current) {
            response[4] = (uint8_t)(A2DP_SDP_RECORD_HANDLE >> 24);
            response[5] = (uint8_t)(A2DP_SDP_RECORD_HANDLE >> 16);
            response[6] = (uint8_t)(A2DP_SDP_RECORD_HANDLE >> 8);
            response[7] = (uint8_t)A2DP_SDP_RECORD_HANDLE;
            responseLength += 4;
        }
        response[responseLength++] = 0;
        return finishPdu(out, (uint8_t)(pdu + 1), transaction, responseLength);
    }

    Element list = {};
    Range ranges[SDP_MAX_RANGES] = {};
    size_t rangeCount = 0;
    if (!readElement(params + offset, parameterLength - offset, &list) ||
        !parseAttributeList(list, ranges, &rangeCount) || maximum < 7) {
        return errorPdu(out, capacity, transaction, SDP_INVALID_SYNTAX);
    }
    offset += list.total;

    uint16_t resume = 0;
    if (parameterLength - offset < 1) {
        return errorPdu(out, capacity, transaction, SDP_INVALID_SYNTAX);
    }
    const uint8_t continuationLength = params[offset++];
    if (continuationLength == 2 && parameterLength - offset >= 2) {
        resume = (uint16_t)((params[offset] << 8) | params[offset + 1]);
    } else if (continuationLength != 0) {
        return errorPdu(out, capacity, transaction, SDP_INVALID_CONTINUATION);
    }

    uint8_t full[320] = {};
    size_t fullLength = 0;
    if (pdu == SDP_ATTRIBUTE_REQUEST) {
        if (!appendAttributeList(ranges, rangeCount, full, sizeof(full), &fullLength)) {
            return 0;
        }
    } else {
        uint8_t lists[300] = {};
        size_t listsLength = 0;
        if (matches && !appendAttributeList(ranges, rangeCount, lists, sizeof(lists), &listsLength)) {
            return 0;
        }
        if (!appendSequenceHeader(full, sizeof(full), &fullLength, listsLength) ||
            !appendRaw(full, sizeof(full), &fullLength, lists, listsLength)) {
            return 0;
        }
    }
    if (resume >= fullLength && !(resume == 0 && fullLength == 0)) {
        return errorPdu(out, capacity, transaction, SDP_INVALID_CONTINUATION);
    }
    size_t chunk = fullLength - resume;
    if (chunk > maximum) {
        chunk = maximum;
    }
    const bool more = resume + chunk < fullLength;
    if (capacity < 5U + 2U + chunk + 3U) {
        return 0;
    }
    uint8_t *response = out + 5;
    writeBe16(response, (uint16_t)chunk);
    memcpy(response + 2, full + resume, chunk);
    size_t responseLength = 2 + chunk;
    if (more) {
        response[responseLength++] = 2;
        writeBe16(response + responseLength, (uint16_t)(resume + chunk));
        responseLength += 2;
    } else {
        response[responseLength++] = 0;
    }
    return finishPdu(out, (uint8_t)(pdu + 1), transaction, responseLength);
}

} // namespace bt
