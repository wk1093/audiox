#pragma once

#include <net/if.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>

// Minimal mDNS (RFC 6762) responder so hosts on the USB link can resolve
// "<hostname>.local" without any DNS configuration.
namespace audiox::mdns {

constexpr uint16_t kPort = 5353;
constexpr uint16_t kTypeA = 1;
constexpr uint16_t kTypePtr = 12;
constexpr uint16_t kTypeAaaa = 28;
constexpr uint16_t kTypeNsec = 47;
constexpr uint16_t kTypeAny = 255;
constexpr uint32_t kTtlSeconds = 120;
constexpr uint32_t kLegacyTtlSeconds = 10;
constexpr size_t kMaxNameLength = 255;
constexpr size_t kMaxReceivePacket = 9000;
constexpr size_t kMaxSendPacket = 1500;

struct Host {
    const char *name;  // fully qualified, e.g. "audiox.local"
    in_addr address;
};

struct Reply {
    size_t length = 0;     // 0 when the packet needs no answer
    bool unicast = false;  // send to the querier instead of the multicast group
};

// Decodes a (possibly compressed) DNS name at *offset into dotted form and
// advances *offset past it. Returns false for malformed names.
bool readName(const uint8_t *packet, size_t length, size_t *offset, char *out, size_t capacity);

// legacyUnicast: the query came from a port other than 5353 (e.g. dig).
Reply buildReply(const uint8_t *query, size_t queryLength, const Host &host, bool legacyUnicast,
                 uint8_t *out, size_t capacity);

size_t buildAnnouncement(const Host &host, uint8_t *out, size_t capacity);

class Responder {
public:
    Responder(const char *interfaceName, const char *hostLabel);
    ~Responder();
    Responder(const Responder &) = delete;
    Responder &operator=(const Responder &) = delete;

    // Non-blocking; call from the main loop. Tracks the interface (it may
    // appear, change address, or lose carrier at any time) and answers queries.
    void poll();

private:
    void refreshInterface(uint64_t nowMs);
    bool openSocket(int interfaceIndex);
    void closeSocket();
    void sendMulticast(const uint8_t *packet, size_t length, uint64_t nowMs);
    void handlePackets(uint64_t nowMs);

    char interfaceName_[IFNAMSIZ] = {};
    char hostName_[kMaxNameLength + 1] = {};
    int fd_ = -1;
    int interfaceIndex_ = -1;
    in_addr address_ = {};
    bool running_ = false;
    bool failureLogged_ = false;
    uint64_t nextCheckMs_ = 0;
    uint64_t nextAnnounceMs_ = 0;
    unsigned announcementsLeft_ = 0;
    uint64_t lastMulticastMs_ = 0;
    size_t lastMulticastLength_ = 0;
    uint8_t lastMulticast_[kMaxSendPacket] = {};
    uint8_t receive_[kMaxReceivePacket] = {};
    uint8_t send_[kMaxSendPacket] = {};
};

} // namespace audiox::mdns
