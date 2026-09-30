#pragma once

#include <cstddef>
#include <cstdint>

// Raw HCI access to the local controller. Everything here blocks; callers
// should only use it from the Bluetooth thread.
namespace bt {

#define BT_NAME_MAX 249
#define BT_ADDRESS_TEXT 18

// Bytes are stored little-endian, exactly as they appear on the wire.
struct Address {
    uint8_t b[6];
};

enum NameQuality : uint8_t {
    NAME_NONE = 0,
    NAME_SHORT = 1,
    NAME_COMPLETE = 2,
};

struct DiscoveredDevice {
    Address address;
    char name[BT_NAME_MAX];
    uint8_t nameQuality;
    bool classic;
    uint8_t pageScanRepMode;
    uint16_t clockOffset;
};

bool parseAddress(const char *text, Address *out);
void formatAddress(const Address &address, char *out, size_t outSize);
bool isZeroAddress(const Address &address);
bool sameAddress(const Address &a, const Address &b);

uint64_t monotonicMs();

// Brings the first adapter up and returns its id, or -1.
int openAdapter();

// Raw socket bound to the adapter that receives every HCI event, or -1.
int openRawSocket(int devId);

int writeCommand(int fd, uint16_t opcode, const uint8_t *params, uint8_t paramLength);

// Sets the classic Bluetooth local name (maximum 248 bytes).
int setLocalName(int fd, const char *name);

// Returns the command status, or -1 on timeout.
int waitForCommand(int fd, uint16_t opcode, int timeoutMs);

// Classic inquiry, remote name lookup, then LE scan. Returns the device count.
size_t scan(int devId, DiscoveredDevice *out, size_t capacity);

} // namespace bt