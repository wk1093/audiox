#include "http/mdns.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace audiox::mdns {
namespace {

constexpr uint16_t kClassIn = 1;
constexpr uint16_t kClassAny = 255;
constexpr uint16_t kCacheFlush = 0x8000;
constexpr uint16_t kQuestionUnicast = 0x8000;
constexpr uint16_t kFlagResponse = 0x8000;
constexpr uint16_t kFlagAuthoritative = 0x0400;
constexpr size_t kHeaderSize = 12;
constexpr unsigned kMaxQuestions = 32;
constexpr unsigned kAnnouncementCount = 2;
constexpr uint64_t kAnnouncementIntervalMs = 1000;
constexpr uint64_t kInterfaceCheckIntervalMs = 1000;
constexpr uint64_t kMulticastRepeatMs = 1000;
constexpr unsigned kMaxPacketsPerPoll = 16;
const char *const kMulticastGroup = "224.0.0.251";

uint16_t read16(const uint8_t *p) {
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

uint64_t monotonicMs() {
    timespec now = {};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<uint64_t>(now.tv_sec) * 1000U + static_cast<uint64_t>(now.tv_nsec) / 1000000U;
}

class Writer {
public:
    Writer(uint8_t *out, size_t capacity) : out_(out), capacity_(capacity) {}

    void u8(uint8_t value) {
        if (length_ + 1 > capacity_) {
            ok_ = false;
            return;
        }
        out_[length_++] = value;
    }
    void u16(uint16_t value) {
        u8(static_cast<uint8_t>(value >> 8));
        u8(static_cast<uint8_t>(value));
    }
    void u32(uint32_t value) {
        u16(static_cast<uint16_t>(value >> 16));
        u16(static_cast<uint16_t>(value));
    }
    void bytes(const void *data, size_t count) {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        for (size_t i = 0; i < count; ++i) {
            u8(p[i]);
        }
    }
    // Names are written uncompressed; replies are tiny, so compression is not worth it.
    void name(const char *dotted) {
        const char *label = dotted;
        while (*label) {
            const char *dot = strchr(label, '.');
            const size_t labelLength = dot ? static_cast<size_t>(dot - label) : strlen(label);
            if (labelLength == 0 || labelLength > 63) {
                ok_ = false;
                return;
            }
            u8(static_cast<uint8_t>(labelLength));
            bytes(label, labelLength);
            if (!dot) {
                break;
            }
            label = dot + 1;
        }
        u8(0);
    }
    void patch16(size_t at, uint16_t value) {
        if (at + 2 <= length_) {
            out_[at] = static_cast<uint8_t>(value >> 8);
            out_[at + 1] = static_cast<uint8_t>(value);
        }
    }

    size_t length() const { return length_; }
    bool ok() const { return ok_; }

private:
    uint8_t *out_;
    size_t capacity_;
    size_t length_ = 0;
    bool ok_ = true;
};

void reverseName(const in_addr &address, char *out, size_t capacity) {
    const uint8_t *octets = reinterpret_cast<const uint8_t *>(&address.s_addr);
    snprintf(out, capacity, "%u.%u.%u.%u.in-addr.arpa", octets[3], octets[2], octets[1], octets[0]);
}

void writeRecordHeader(Writer &w, const char *name, uint16_t type, uint16_t recordClass, uint32_t ttl) {
    w.name(name);
    w.u16(type);
    w.u16(recordClass);
    w.u32(ttl);
}

void writeA(Writer &w, const Host &host, uint16_t recordClass, uint32_t ttl) {
    writeRecordHeader(w, host.name, kTypeA, recordClass, ttl);
    w.u16(4);
    w.bytes(&host.address.s_addr, 4);
}

// Restricted-form NSEC (RFC 6762 section 6.1): the host name owns only an A record.
void writeNsec(Writer &w, const Host &host, uint16_t recordClass, uint32_t ttl) {
    writeRecordHeader(w, host.name, kTypeNsec, recordClass, ttl);
    const size_t lengthAt = w.length();
    w.u16(0);
    const size_t start = w.length();
    w.name(host.name);
    w.u8(0);     // window block 0
    w.u8(1);     // bitmap length
    w.u8(0x40);  // type 1 (A)
    w.patch16(lengthAt, static_cast<uint16_t>(w.length() - start));
}

void writePtr(Writer &w, const Host &host, const char *reverse, uint16_t recordClass, uint32_t ttl) {
    writeRecordHeader(w, reverse, kTypePtr, recordClass, ttl);
    const size_t lengthAt = w.length();
    w.u16(0);
    const size_t start = w.length();
    w.name(host.name);
    w.patch16(lengthAt, static_cast<uint16_t>(w.length() - start));
}

struct Question {
    char name[kMaxNameLength + 1];
    uint16_t type;
    uint16_t recordClass;
};

} // namespace

bool readName(const uint8_t *packet, size_t length, size_t *offset, char *out, size_t capacity) {
    if (!packet || !offset || !out || capacity == 0) {
        return false;
    }
    size_t position = *offset;
    size_t written = 0;
    size_t resumeAt = 0;
    bool jumped = false;
    out[0] = '\0';

    for (;;) {
        if (position >= length) {
            return false;
        }
        const uint8_t labelLength = packet[position];
        if ((labelLength & 0xC0) == 0xC0) {
            if (position + 1 >= length) {
                return false;
            }
            const size_t target = static_cast<size_t>(((labelLength & 0x3F) << 8) | packet[position + 1]);
            // Pointers must point strictly backwards, which also rules out loops.
            if (target >= position) {
                return false;
            }
            if (!jumped) {
                resumeAt = position + 2;
                jumped = true;
            }
            position = target;
            continue;
        }
        if ((labelLength & 0xC0) != 0) {
            return false;
        }
        if (labelLength == 0) {
            *offset = jumped ? resumeAt : position + 1;
            out[written] = '\0';
            return true;
        }
        if (position + 1 + labelLength > length) {
            return false;
        }
        const size_t needed = (written ? 1U : 0U) + labelLength;
        if (written + needed + 1 > capacity || written + needed > kMaxNameLength) {
            return false;
        }
        if (written) {
            out[written++] = '.';
        }
        for (size_t i = 0; i < labelLength; ++i) {
            const char c = static_cast<char>(packet[position + 1 + i]);
            if (c == '.' || c == '\0') {
                return false;
            }
            out[written++] = c;
        }
        position += 1 + labelLength;
    }
}

Reply buildReply(const uint8_t *query, size_t queryLength, const Host &host, bool legacyUnicast,
                 uint8_t *out, size_t capacity) {
    Reply reply;
    if (!query || queryLength < kHeaderSize || !host.name || !out) {
        return reply;
    }
    const uint16_t flags = read16(query + 2);
    if ((flags & kFlagResponse) != 0 || ((flags >> 11) & 0xF) != 0) {
        return reply;
    }
    const unsigned questionCount = read16(query + 4);

    char reverse[64];
    reverseName(host.address, reverse, sizeof(reverse));

    Question answered[kMaxQuestions];
    unsigned answeredCount = 0;
    bool wantA = false;
    bool wantNsec = false;
    bool wantPtr = false;
    bool multicastRequested = false;

    size_t offset = kHeaderSize;
    for (unsigned i = 0; i < questionCount && i < kMaxQuestions; ++i) {
        Question question = {};
        if (!readName(query, queryLength, &offset, question.name, sizeof(question.name)) ||
            offset + 4 > queryLength) {
            break;
        }
        question.type = read16(query + offset);
        const uint16_t rawClass = read16(query + offset + 2);
        offset += 4;
        question.recordClass = rawClass & ~kQuestionUnicast;
        if (question.recordClass != kClassIn && question.recordClass != kClassAny) {
            continue;
        }

        bool matched = false;
        if (strcasecmp(question.name, host.name) == 0) {
            if (question.type == kTypeA || question.type == kTypeAny) {
                wantA = true;
            } else {
                wantNsec = true;
            }
            matched = true;
        } else if (strcasecmp(question.name, reverse) == 0 &&
                   (question.type == kTypePtr || question.type == kTypeAny)) {
            wantPtr = true;
            matched = true;
        }
        if (matched) {
            multicastRequested = multicastRequested || (rawClass & kQuestionUnicast) == 0;
            answered[answeredCount++] = question;
        }
    }
    if (answeredCount == 0) {
        return reply;
    }

    // Legacy unicast replies echo the ID and questions, use a short TTL, and
    // must not set the cache-flush bit (RFC 6762 section 6.7).
    const uint32_t ttl = legacyUnicast ? kLegacyTtlSeconds : kTtlSeconds;
    const uint16_t recordClass = legacyUnicast ? kClassIn : static_cast<uint16_t>(kClassIn | kCacheFlush);
    const bool additionalA = !wantA;
    const bool additionalNsec = !wantNsec;
    const uint16_t answerCount = static_cast<uint16_t>(wantA + wantNsec + wantPtr);
    const uint16_t additionalCount = static_cast<uint16_t>(additionalA + additionalNsec);

    Writer w(out, capacity);
    w.u16(legacyUnicast ? read16(query) : 0);
    w.u16(kFlagResponse | kFlagAuthoritative);
    w.u16(legacyUnicast ? static_cast<uint16_t>(answeredCount) : 0);
    w.u16(answerCount);
    w.u16(0);
    w.u16(additionalCount);
    if (legacyUnicast) {
        for (unsigned i = 0; i < answeredCount; ++i) {
            w.name(answered[i].name);
            w.u16(answered[i].type);
            w.u16(answered[i].recordClass);
        }
    }
    if (wantA) {
        writeA(w, host, recordClass, ttl);
    }
    if (wantNsec) {
        writeNsec(w, host, recordClass, ttl);
    }
    if (wantPtr) {
        writePtr(w, host, reverse, recordClass, ttl);
    }
    if (additionalA) {
        writeA(w, host, recordClass, ttl);
    }
    if (additionalNsec) {
        writeNsec(w, host, recordClass, ttl);
    }
    if (!w.ok()) {
        return reply;
    }
    reply.length = w.length();
    reply.unicast = legacyUnicast || !multicastRequested;
    return reply;
}

size_t buildAnnouncement(const Host &host, uint8_t *out, size_t capacity) {
    if (!host.name || !out) {
        return 0;
    }
    char reverse[64];
    reverseName(host.address, reverse, sizeof(reverse));
    const uint16_t recordClass = static_cast<uint16_t>(kClassIn | kCacheFlush);

    Writer w(out, capacity);
    w.u16(0);
    w.u16(kFlagResponse | kFlagAuthoritative);
    w.u16(0);
    w.u16(2);
    w.u16(0);
    w.u16(1);
    writeA(w, host, recordClass, kTtlSeconds);
    writePtr(w, host, reverse, recordClass, kTtlSeconds);
    writeNsec(w, host, recordClass, kTtlSeconds);
    return w.ok() ? w.length() : 0;
}

Responder::Responder(const char *interfaceName, const char *hostLabel) {
    snprintf(interfaceName_, sizeof(interfaceName_), "%s", interfaceName ? interfaceName : "");
    snprintf(hostName_, sizeof(hostName_), "%s.local", hostLabel ? hostLabel : "audiox");
}

Responder::~Responder() {
    closeSocket();
}

void Responder::closeSocket() {
    if (fd_ >= 0) {
        close(fd_);
    }
    fd_ = -1;
    interfaceIndex_ = -1;
    announcementsLeft_ = 0;
    lastMulticastLength_ = 0;
}

bool Responder::openSocket(int interfaceIndex) {
    const int fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        if (!failureLogged_) {
            printf("[MDNS] [WARN] socket failed: %s\n", strerror(errno));
        }
        return false;
    }
    const int yes = 1;
    const int ttl = 255;
    const unsigned char loop = 0;
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#ifdef SO_REUSEPORT
    (void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &yes, sizeof(yes));
#endif
    // Only the USB link is served; queries arriving on other interfaces are never seen.
    (void)setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interfaceName_, static_cast<socklen_t>(strlen(interfaceName_)));

    sockaddr_in bindAddress = {};
    bindAddress.sin_family = AF_INET;
    bindAddress.sin_port = htons(kPort);
    bindAddress.sin_addr.s_addr = htonl(INADDR_ANY);

    ip_mreqn membership = {};
    inet_pton(AF_INET, kMulticastGroup, &membership.imr_multiaddr);
    membership.imr_ifindex = interfaceIndex;

    const char *step = nullptr;
    if (bind(fd, reinterpret_cast<sockaddr *>(&bindAddress), sizeof(bindAddress)) < 0) {
        step = "bind";
    } else if (setsockopt(fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)) < 0) {
        step = "join multicast group";
    } else if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_IF, &membership, sizeof(membership)) < 0) {
        step = "select multicast interface";
    } else if (setsockopt(fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)) < 0 ||
               setsockopt(fd, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl)) < 0 ||
               setsockopt(fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop)) < 0) {
        step = "configure multicast";
    }
    if (step) {
        if (!failureLogged_) {
            printf("[MDNS] [WARN] %s on %s failed: %s\n", step, interfaceName_, strerror(errno));
        }
        close(fd);
        return false;
    }
    fd_ = fd;
    interfaceIndex_ = interfaceIndex;
    return true;
}

void Responder::refreshInterface(uint64_t nowMs) {
    const int ctl = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (ctl < 0) {
        return;
    }
    ifreq request = {};
    snprintf(request.ifr_name, sizeof(request.ifr_name), "%s", interfaceName_);
    int index = -1;
    short flags = 0;
    in_addr address = {};
    bool ready = false;
    if (ioctl(ctl, SIOCGIFINDEX, &request) == 0) {
        index = request.ifr_ifindex;
        if (ioctl(ctl, SIOCGIFFLAGS, &request) == 0) {
            flags = request.ifr_flags;
            if ((flags & IFF_UP) && ioctl(ctl, SIOCGIFADDR, &request) == 0) {
                address = reinterpret_cast<sockaddr_in *>(&request.ifr_addr)->sin_addr;
                ready = address.s_addr != 0;
            }
        }
    }
    close(ctl);

    if (!ready) {
        if (fd_ >= 0) {
            printf("[MDNS] %s is down or has no IPv4 address; responder paused\n", interfaceName_);
            closeSocket();
        }
        running_ = false;
        return;
    }

    const bool running = (flags & IFF_RUNNING) != 0;
    if (fd_ < 0 || index != interfaceIndex_ || address.s_addr != address_.s_addr) {
        closeSocket();
        address_ = address;
        if (!openSocket(index)) {
            failureLogged_ = true;
            return;
        }
        failureLogged_ = false;
        char text[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &address_, text, sizeof(text));
        printf("[MDNS] answering %s -> %s on %s\n", hostName_, text, interfaceName_);
        announcementsLeft_ = kAnnouncementCount;
        nextAnnounceMs_ = nowMs;
    } else if (running && !running_) {
        // A host was just plugged in; tell it about us straight away.
        announcementsLeft_ = kAnnouncementCount;
        nextAnnounceMs_ = nowMs;
    }
    running_ = running;
}

void Responder::sendMulticast(const uint8_t *packet, size_t length, uint64_t nowMs) {
    // Do not repeat an identical multicast answer within a second (RFC 6762 section 6).
    if (length == lastMulticastLength_ && nowMs - lastMulticastMs_ < kMulticastRepeatMs &&
        memcmp(packet, lastMulticast_, length) == 0) {
        return;
    }
    sockaddr_in group = {};
    group.sin_family = AF_INET;
    group.sin_port = htons(kPort);
    inet_pton(AF_INET, kMulticastGroup, &group.sin_addr);
    if (sendto(fd_, packet, length, 0, reinterpret_cast<sockaddr *>(&group), sizeof(group)) < 0) {
        if (errno != EAGAIN && errno != ENETDOWN && errno != ENETUNREACH) {
            printf("[MDNS] [WARN] multicast send failed: %s\n", strerror(errno));
        }
        return;
    }
    memcpy(lastMulticast_, packet, length);
    lastMulticastLength_ = length;
    lastMulticastMs_ = nowMs;
}

void Responder::handlePackets(uint64_t nowMs) {
    const Host host = {hostName_, address_};
    for (unsigned i = 0; i < kMaxPacketsPerPoll; ++i) {
        sockaddr_in source = {};
        socklen_t sourceLength = sizeof(source);
        const ssize_t received = recvfrom(fd_, receive_, sizeof(receive_), 0,
                                          reinterpret_cast<sockaddr *>(&source), &sourceLength);
        if (received < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                printf("[MDNS] [WARN] receive failed: %s\n", strerror(errno));
            }
            return;
        }
        if (source.sin_addr.s_addr == address_.s_addr) {
            continue;
        }
        const bool legacy = ntohs(source.sin_port) != kPort;
        const Reply reply = buildReply(receive_, static_cast<size_t>(received), host, legacy, send_, sizeof(send_));
        if (reply.length == 0) {
            continue;
        }
        if (reply.unicast) {
            (void)sendto(fd_, send_, reply.length, 0, reinterpret_cast<sockaddr *>(&source), sourceLength);
        } else {
            sendMulticast(send_, reply.length, nowMs);
        }
    }
}

void Responder::poll() {
    const uint64_t nowMs = monotonicMs();
    if (nowMs >= nextCheckMs_) {
        nextCheckMs_ = nowMs + kInterfaceCheckIntervalMs;
        refreshInterface(nowMs);
    }
    if (fd_ < 0) {
        return;
    }
    if (announcementsLeft_ > 0 && nowMs >= nextAnnounceMs_) {
        const size_t length = buildAnnouncement({hostName_, address_}, send_, sizeof(send_));
        if (length > 0) {
            sendMulticast(send_, length, nowMs);
        }
        --announcementsLeft_;
        nextAnnounceMs_ = nowMs + kAnnouncementIntervalMs;
    }
    handlePackets(nowMs);
}

} // namespace audiox::mdns
