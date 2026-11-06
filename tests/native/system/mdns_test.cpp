#include "http/mdns.hpp"

#include <arpa/inet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <string>
#include <vector>

using namespace audiox::mdns;

namespace {

int failures = 0;

#define CHECK(condition)                                                        \
    do {                                                                        \
        if (!(condition)) {                                                     \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures;                                                         \
        }                                                                       \
    } while (0)

uint16_t get16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
uint32_t get32(const uint8_t *p) { return (static_cast<uint32_t>(get16(p)) << 16) | get16(p + 2); }

struct Record {
    std::string name;
    uint16_t type = 0;
    uint16_t recordClass = 0;
    uint32_t ttl = 0;
    std::vector<uint8_t> data;
    size_t dataOffset = 0;
};

struct Message {
    uint16_t id = 0;
    uint16_t flags = 0;
    std::vector<std::string> questions;
    std::vector<Record> answers;
    std::vector<Record> additional;
    bool ok = false;
};

Message parse(const uint8_t *packet, size_t length) {
    Message message;
    if (length < 12) {
        return message;
    }
    message.id = get16(packet);
    message.flags = get16(packet + 2);
    const unsigned qd = get16(packet + 4);
    const unsigned an = get16(packet + 6);
    const unsigned ns = get16(packet + 8);
    const unsigned ar = get16(packet + 10);
    size_t offset = 12;
    char name[kMaxNameLength + 1];
    for (unsigned i = 0; i < qd; ++i) {
        if (!readName(packet, length, &offset, name, sizeof(name)) || offset + 4 > length) {
            return message;
        }
        message.questions.push_back(name);
        offset += 4;
    }
    for (unsigned i = 0; i < an + ns + ar; ++i) {
        Record record;
        if (!readName(packet, length, &offset, name, sizeof(name)) || offset + 10 > length) {
            return message;
        }
        record.name = name;
        record.type = get16(packet + offset);
        record.recordClass = get16(packet + offset + 2);
        record.ttl = get32(packet + offset + 4);
        const uint16_t rdLength = get16(packet + offset + 8);
        offset += 10;
        if (offset + rdLength > length) {
            return message;
        }
        record.dataOffset = offset;
        record.data.assign(packet + offset, packet + offset + rdLength);
        offset += rdLength;
        if (i < an) {
            message.answers.push_back(record);
        } else if (i >= an + ns) {
            message.additional.push_back(record);
        }
    }
    message.ok = offset == length;
    return message;
}

std::vector<uint8_t> encodeName(const char *dotted) {
    std::vector<uint8_t> out;
    std::string name(dotted);
    size_t start = 0;
    while (start < name.size()) {
        size_t dot = name.find('.', start);
        if (dot == std::string::npos) {
            dot = name.size();
        }
        out.push_back(static_cast<uint8_t>(dot - start));
        out.insert(out.end(), name.begin() + static_cast<long>(start), name.begin() + static_cast<long>(dot));
        start = dot + 1;
    }
    out.push_back(0);
    return out;
}

struct Q {
    const char *name;
    uint16_t type;
    bool unicast;
};

std::vector<uint8_t> query(uint16_t id, std::vector<Q> questions, uint16_t flags = 0) {
    std::vector<uint8_t> out = {static_cast<uint8_t>(id >> 8), static_cast<uint8_t>(id),
                                static_cast<uint8_t>(flags >> 8), static_cast<uint8_t>(flags),
                                0, static_cast<uint8_t>(questions.size()), 0, 0, 0, 0, 0, 0};
    for (const Q &q : questions) {
        const std::vector<uint8_t> name = encodeName(q.name);
        out.insert(out.end(), name.begin(), name.end());
        out.push_back(static_cast<uint8_t>(q.type >> 8));
        out.push_back(static_cast<uint8_t>(q.type));
        out.push_back(q.unicast ? 0x80 : 0x00);
        out.push_back(0x01);
    }
    return out;
}

Host testHost() {
    Host host = {"audiox.local", {}};
    inet_pton(AF_INET, "172.31.73.1", &host.address);
    return host;
}

Message reply(const std::vector<uint8_t> &packet, bool legacy, Reply *result) {
    static uint8_t out[kMaxSendPacket];
    *result = buildReply(packet.data(), packet.size(), testHost(), legacy, out, sizeof(out));
    return parse(out, result->length);
}

bool isAddress(const Record &record) {
    return record.type == kTypeA && record.data.size() == 4 && record.data[0] == 172 &&
        record.data[1] == 31 && record.data[2] == 73 && record.data[3] == 1;
}

bool isNsecForA(const uint8_t *packet, size_t length, const Record &record) {
    if (record.type != kTypeNsec) {
        return false;
    }
    size_t offset = record.dataOffset;
    char next[kMaxNameLength + 1];
    if (!readName(packet, length, &offset, next, sizeof(next)) || strcasecmp(next, "audiox.local") != 0) {
        return false;
    }
    const size_t bitmapAt = offset - record.dataOffset;
    return record.data.size() == bitmapAt + 3 && record.data[bitmapAt] == 0 &&
        record.data[bitmapAt + 1] == 1 && record.data[bitmapAt + 2] == 0x40;
}

void testMulticastAQuery() {
    Reply result;
    uint8_t out[kMaxSendPacket];
    const std::vector<uint8_t> packet = query(0x1234, {{"AudioX.LOCAL", kTypeA, false}});
    result = buildReply(packet.data(), packet.size(), testHost(), false, out, sizeof(out));
    const Message message = parse(out, result.length);
    CHECK(result.length > 0);
    CHECK(!result.unicast);
    CHECK(message.ok);
    CHECK(message.id == 0);
    CHECK(message.flags == 0x8400);
    CHECK(message.questions.empty());
    CHECK(message.answers.size() == 1);
    CHECK(message.answers.size() == 1 && isAddress(message.answers[0]));
    CHECK(message.answers.size() == 1 && message.answers[0].name == "audiox.local");
    CHECK(message.answers.size() == 1 && message.answers[0].recordClass == 0x8001);
    CHECK(message.answers.size() == 1 && message.answers[0].ttl == kTtlSeconds);
    CHECK(message.additional.size() == 1 && isNsecForA(out, result.length, message.additional[0]));
}

void testUnicastRequest() {
    Reply result;
    const Message message = reply(query(1, {{"audiox.local", kTypeA, true}}), false, &result);
    CHECK(result.length > 0 && result.unicast);
    CHECK(message.ok && message.answers.size() == 1);

    // One multicast question in the packet makes the whole reply multicast.
    const Message mixed = reply(query(1, {{"audiox.local", kTypeA, true},
                                          {"audiox.local", kTypeAaaa, false}}), false, &result);
    CHECK(result.length > 0 && !result.unicast);
    CHECK(mixed.ok && mixed.answers.size() == 2 && mixed.additional.empty());
}

void testLegacyUnicast() {
    Reply result;
    const Message message = reply(query(0xBEEF, {{"audiox.local", kTypeA, false}}), true, &result);
    CHECK(result.length > 0 && result.unicast);
    CHECK(message.ok);
    CHECK(message.id == 0xBEEF);
    CHECK(message.questions.size() == 1 && message.questions[0] == "audiox.local");
    CHECK(message.answers.size() == 1 && isAddress(message.answers[0]));
    CHECK(message.answers.size() == 1 && message.answers[0].recordClass == 0x0001);
    CHECK(message.answers.size() == 1 && message.answers[0].ttl == kLegacyTtlSeconds);
}

void testNegativeAaaa() {
    Reply result;
    uint8_t out[kMaxSendPacket];
    const std::vector<uint8_t> packet = query(0, {{"audiox.local", kTypeAaaa, false}});
    result = buildReply(packet.data(), packet.size(), testHost(), false, out, sizeof(out));
    const Message message = parse(out, result.length);
    CHECK(message.ok);
    CHECK(message.answers.size() == 1 && isNsecForA(out, result.length, message.answers[0]));
    CHECK(message.additional.size() == 1 && isAddress(message.additional[0]));
}

void testAnyAndReverse() {
    Reply result;
    Message message = reply(query(0, {{"audiox.local", kTypeAny, false}}), false, &result);
    CHECK(message.ok && message.answers.size() == 1 && isAddress(message.answers[0]));

    uint8_t out[kMaxSendPacket];
    const std::vector<uint8_t> packet = query(0, {{"1.73.31.172.in-addr.arpa", kTypePtr, false}});
    result = buildReply(packet.data(), packet.size(), testHost(), false, out, sizeof(out));
    message = parse(out, result.length);
    CHECK(message.ok && message.answers.size() == 1);
    if (message.answers.size() == 1) {
        size_t offset = message.answers[0].dataOffset;
        char target[kMaxNameLength + 1];
        CHECK(message.answers[0].type == kTypePtr);
        CHECK(readName(out, result.length, &offset, target, sizeof(target)));
        CHECK(strcmp(target, "audiox.local") == 0);
    }
    CHECK(message.additional.size() == 2);
}

void testIgnoredPackets() {
    Reply result;
    reply(query(0, {{"other.local", kTypeA, false}}), false, &result);
    CHECK(result.length == 0);
    reply(query(0, {{"audiox.local.example", kTypeA, false}}), false, &result);
    CHECK(result.length == 0);
    reply(query(0, {{"audiox.local", kTypeA, false}}, 0x8400), false, &result);
    CHECK(result.length == 0);  // responses
    reply(query(0, {{"audiox.local", kTypeA, false}}, 0x2800), false, &result);
    CHECK(result.length == 0);  // non-standard opcode
    reply(query(0, {{"1.73.31.172.in-addr.arpa", kTypeA, false}}), false, &result);
    CHECK(result.length == 0);

    std::vector<uint8_t> chaos = query(0, {{"audiox.local", kTypeA, false}});
    chaos[chaos.size() - 1] = 3;  // CH class
    reply(chaos, false, &result);
    CHECK(result.length == 0);
}

void testCompressedQuestions() {
    // Second question "audiox.local" is a pointer to the first question's name.
    std::vector<uint8_t> packet = query(0, {{"other.local", kTypeA, false}});
    packet[5] = 2;
    const std::vector<uint8_t> label = {6, 'a', 'u', 'd', 'i', 'o', 'x', 0xC0, 18};
    packet.insert(packet.end(), label.begin(), label.end());
    packet.insert(packet.end(), {0, 1, 0, 1});
    Reply result;
    const Message message = reply(packet, false, &result);
    CHECK(result.length > 0);
    CHECK(message.ok && message.answers.size() == 1 && isAddress(message.answers[0]));
}

void testMalformedPackets() {
    uint8_t out[kMaxSendPacket];
    const Host host = testHost();

    uint8_t loop[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 12, 0, 1, 0, 1};
    CHECK(buildReply(loop, sizeof(loop), host, false, out, sizeof(out)).length == 0);

    uint8_t forward[] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0xC0, 14, 0, 1, 0, 1};
    CHECK(buildReply(forward, sizeof(forward), host, false, out, sizeof(out)).length == 0);

    std::vector<uint8_t> packet = query(0, {{"audiox.local", kTypeA, false}});
    for (size_t length = 0; length < packet.size(); ++length) {
        CHECK(buildReply(packet.data(), length, host, false, out, sizeof(out)).length == 0);
    }
    packet[12] = 0x80;  // reserved label type
    CHECK(buildReply(packet.data(), packet.size(), host, false, out, sizeof(out)).length == 0);

    uint8_t garbage[512];
    srand(7);
    for (int round = 0; round < 20000; ++round) {
        const size_t length = static_cast<size_t>(rand()) % sizeof(garbage);
        for (size_t i = 0; i < length; ++i) {
            garbage[i] = static_cast<uint8_t>(rand());
        }
        const Reply result = buildReply(garbage, length, host, round % 2 == 0, out, sizeof(out));
        CHECK(result.length <= sizeof(out));
    }

    // A buffer too small for the reply must produce no reply, not a truncated one.
    packet = query(0, {{"audiox.local", kTypeA, false}});
    CHECK(buildReply(packet.data(), packet.size(), host, false, out, 40).length == 0);
}

void testAnnouncement() {
    uint8_t out[kMaxSendPacket];
    const size_t length = buildAnnouncement(testHost(), out, sizeof(out));
    const Message message = parse(out, length);
    CHECK(length > 0);
    CHECK(message.ok && message.id == 0 && message.flags == 0x8400);
    CHECK(message.answers.size() == 2 && isAddress(message.answers[0]));
    CHECK(message.answers.size() == 2 && message.answers[1].type == kTypePtr &&
          message.answers[1].name == "1.73.31.172.in-addr.arpa");
    CHECK(message.additional.size() == 1 && isNsecForA(out, length, message.additional[0]));
    for (const Record &record : message.answers) {
        CHECK(record.recordClass == 0x8001 && record.ttl == kTtlSeconds);
    }
}

} // namespace

int main() {
    testMulticastAQuery();
    testUnicastRequest();
    testLegacyUnicast();
    testNegativeAaaa();
    testAnyAndReverse();
    testIgnoredPackets();
    testCompressedQuestions();
    testMalformedPackets();
    testAnnouncement();
    if (failures != 0) {
        fprintf(stderr, "mdns tests failed: %d\n", failures);
        return 1;
    }
    printf("mdns responder tests passed\n");
    return 0;
}
