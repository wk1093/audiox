#include "bluetooth/hci.hpp"

#include <cassert>
#include <cstdio>
#include <cstring>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

static int controller[2] = {-1, -1};
static int scanFd = -1;
static std::vector<uint16_t> commands;
static const bt::Address namedAddress = {{1, 2, 3, 4, 5, 6}};
static const bt::Address unnamedAddress = {{2, 2, 3, 4, 5, 6}};
static bool includeName = true;

extern "C" int __real_socket(int, int, int);
extern "C" int __wrap_socket(int domain, int type, int protocol) {
    if (domain != 31) return __real_socket(domain, type, protocol);
    assert(type == (SOCK_RAW | SOCK_CLOEXEC) && protocol == 1);
    scanFd = dup(controller[0]);
    assert(scanFd >= 0);
    return scanFd;
}

extern "C" int __real_bind(int, const sockaddr *, socklen_t);
extern "C" int __wrap_bind(int fd, const sockaddr *address, socklen_t length) {
    if (fd != scanFd) return __real_bind(fd, address, length);
    assert(address->sa_family == 31);
    return 0;
}

extern "C" int __real_setsockopt(int, int, int, const void *, socklen_t);
extern "C" int __wrap_setsockopt(int fd, int level, int option, const void *value, socklen_t length) {
    if (fd != scanFd) return __real_setsockopt(fd, level, option, value, length);
    assert(level == 0 && option == 2 && length > 0);
    return 0;
}

extern "C" ssize_t __real_write(int, const void *, size_t);
static void sendPacket(const std::vector<uint8_t> &packet) {
    assert(__real_write(controller[1], packet.data(), packet.size()) == (ssize_t)packet.size());
}

extern "C" ssize_t __wrap_write(int fd, const void *bytes, size_t count) {
    if (fd != scanFd) return __real_write(fd, bytes, count);
    const auto *packet = static_cast<const uint8_t *>(bytes);
    assert(count >= 4 && packet[0] == 1 && count == (size_t)packet[3] + 4);
    const uint16_t opcode = packet[1] | (packet[2] << 8);
    commands.push_back(opcode);
    if (opcode == 0x0C45) {
        assert(packet[4] == 2);
        sendPacket({4, 0x0E, 4, 1, 0x45, 0x0C, 0});
    } else if (opcode == 0x0401) {
        // An unrelated LE report must not create a discovery result.
        sendPacket({4, 0x3E, 12, 2, 1, 0, 0, 9, 8, 7, 6, 5, 4, 0, 0});
        std::vector<uint8_t> eir(18, 0);
        eir[0] = 4;
        eir[1] = 0x2F;
        eir[3] = 1;
        memcpy(eir.data() + 4, namedAddress.b, 6);
        eir[10] = 1;
        if (includeName) {
            const uint8_t name[] = {8, 9, 'S', 'p', 'e', 'a', 'k', 'e', 'r', 0};
            eir.insert(eir.end(), name, name + sizeof(name));
        }
        eir[2] = (uint8_t)(eir.size() - 3);
        sendPacket(eir);
        sendPacket(eir); // Duplicate results retain one device.
        std::vector<uint8_t> standard(18, 0);
        standard[0] = 4;
        standard[1] = 2;
        standard[2] = 15;
        standard[3] = 1;
        memcpy(standard.data() + 4, unnamedAddress.b, 6);
        standard[10] = 1;
        sendPacket(standard);
        sendPacket({4, 1, 1, 0});
    } else if (opcode == 0x0419) {
        std::vector<uint8_t> reply(10, 0);
        reply[0] = 4;
        reply[1] = 7;
        memcpy(reply.data() + 4, packet + 4, 6);
        if (!memcmp(packet + 4, namedAddress.b, 6)) {
            const char name[] = "Resolved speaker";
            reply.insert(reply.end(), name, name + sizeof(name));
        }
        reply[2] = (uint8_t)(reply.size() - 3);
        sendPacket(reply);
    } else {
        // In particular, discovery must never issue LE scan commands.
        assert(false && "unexpected Bluetooth discovery command");
    }
    return (ssize_t)count;
}

static void checkScan(size_t capacity, bool withName) {
    assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, controller) == 0);
    commands.clear();
    includeName = withName;
    bt::DiscoveredDevice devices[4] = {};
    const size_t count = bt::scan(0, devices, capacity);
    scanFd = -1;
    assert(count == (capacity == 1 ? 1U : 2U));
    assert(devices[0].classic && bt::sameAddress(devices[0].address, namedAddress));
    assert(!strcmp(devices[0].name, withName ? "Speaker" : "Resolved speaker"));
    assert(devices[0].nameQuality == bt::NAME_COMPLETE);
    if (count > 1) {
        assert(devices[1].classic && bt::sameAddress(devices[1].address, unnamedAddress));
        assert(!devices[1].name[0]);
    }
    assert(commands[0] == 0x0C45 && commands[1] == 0x0401);
    assert(commands.size() == 2 + (withName ? 0U : 1U) + (count > 1 ? 1U : 0U));
    close(controller[0]);
    close(controller[1]);
}

int main() {
    assert(bt::scan(0, nullptr, 1) == 0);
    bt::DiscoveredDevice ignored{};
    assert(bt::scan(0, &ignored, 0) == 0);
    checkScan(4, true);
    checkScan(4, false);
    checkScan(1, true);
    puts("Classic-only discovery, name lookup, deduplication, and capacity checks passed");
}
