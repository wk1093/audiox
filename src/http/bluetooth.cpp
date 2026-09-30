#include "http/bluetooth.hpp"

#include "defs.hpp"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace hci {

struct Address {
    uint8_t b[6];
};

struct DeviceRequest {
    uint16_t devId;
    uint32_t devOpt;
};

struct DeviceListRequest {
    uint16_t devNum;
    DeviceRequest devReq[16];
};

struct InquiryRequest {
    uint16_t devId;
    uint16_t length;
    uint8_t numResponses;
    uint8_t *responses;
};

struct InquiryInfo {
    Address address;
    uint8_t scanMode;
    uint8_t reserved[3];
    uint32_t clockOffset;
};

struct SocketAddress {
    unsigned short family;
    uint16_t devId;
    uint16_t channel;
};

static_assert(sizeof(DeviceRequest) == 8, "unexpected HCI device request layout");
static_assert(offsetof(DeviceListRequest, devReq) == 4, "unexpected HCI device list layout");

} // namespace hci

#define AUDIOX_HCI_DEV_UP 1U
#define AUDIOX_HCI_INIT (1U << 16)
#define AUDIOX_HCI_SETUP (1U << 18)
#define AUDIOX_HCIDEVUP _IOW('H', 201, int)
#define AUDIOX_HCIGETDEVLIST _IOR('H', 210, int)
#define AUDIOX_HCIINQUIRY _IOR('H', 211, int)
#define AUDIOX_AF_BLUETOOTH 31
#define AUDIOX_BTPROTO_HCI 1
#define AUDIOX_HCI_COMMAND_PKT 0x01
#define AUDIOX_HCI_EVENT_PKT 0x04
#define AUDIOX_HCI_INQUIRY 0x0401
#define AUDIOX_EVT_INQUIRY_COMPLETE 0x01
#define AUDIOX_EVT_INQUIRY_RESULT 0x02
#define AUDIOX_EVT_REMOTE_NAME_COMPLETE 0x07
#define AUDIOX_EVT_CMD_COMPLETE 0x0E
#define AUDIOX_EVT_CMD_STATUS 0x0F
#define AUDIOX_EVT_INQUIRY_RESULT_RSSI 0x22
#define AUDIOX_EVT_EXTENDED_INQUIRY_RESULT 0x2F
#define AUDIOX_HCI_REMOTE_NAME_REQUEST 0x0419
#define AUDIOX_HCI_REMOTE_NAME_CANCEL 0x041A
#define AUDIOX_HCI_WRITE_INQUIRY_MODE 0x0C45
#define AUDIOX_BT_NAME_MAX 249
#define AUDIOX_EVT_LE_META_EVENT 0x3E
#define AUDIOX_LE_ADVERTISING_REPORT 0x02
#define AUDIOX_LE_EXTENDED_ADVERTISING_REPORT 0x0D
#define AUDIOX_HCI_CHANNEL_RAW 0
#define AUDIOX_SOL_HCI 0
#define AUDIOX_HCI_FILTER 2
#define AUDIOX_LE_SET_SCAN_PARAMS 0x200B
#define AUDIOX_LE_SET_SCAN_ENABLE 0x200C
#define AUDIOX_BT_MAX_DEVICES 64

namespace {

static uint64_t monotonicMilliseconds() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}
struct HciFilter {
    uint32_t typeMask;
    uint32_t eventMask[2];
    uint16_t opcode;
};


// static size_t appendJsonString(char *out, size_t outSize, size_t used, const char *text) {
//     if (!out || used >= outSize) {
//         return used;
//     }
//     for (const unsigned char *p = (const unsigned char *)(text ? text : ""); *p; ++p) {
//         const char *escaped = nullptr;
//         char encoded[7] = {};
//         if (*p == '"' || *p == '\\') {
//             encoded[0] = '\\';
//             encoded[1] = (char)*p;
//             escaped = encoded;
//         } else if (*p < 0x20) {
//             snprintf(encoded, sizeof(encoded), "\\u%04x", (unsigned)*p);
//             escaped = encoded;
//         }
//         const char *value = escaped ? escaped : (const char *)p;
//         size_t length = escaped ? strlen(escaped) : 1;
//         if (used + length + 1 >= outSize) {
//             break;
//         }
//         memcpy(out + used, value, length);
//         used += length;
//     }
//     out[used] = '\0';
//     return used;
// }

static void formatAddress(const hci::Address &address, char *out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X",
             address.b[5], address.b[4], address.b[3],
             address.b[2], address.b[1], address.b[0]);
}

static int configureEventFilter(int fd) {
    HciFilter filter = {};
    filter.typeMask = 1U << AUDIOX_HCI_EVENT_PKT;
    filter.eventMask[0] = 0xFFFFFFFFU;
    filter.eventMask[1] = 0xFFFFFFFFU;
    return setsockopt(fd,
                      AUDIOX_SOL_HCI,
                      AUDIOX_HCI_FILTER,
                      &filter,
                      sizeof(filter)) == 0 ? RET_OK : RET_ERR;
}

// static void logDirectoryEntries(const char *path) {
//     DIR *dir = opendir(path);
//     if (!dir) {
//         printf("[BT] [INFO] %s: unavailable: %s\n", path, strerror(errno));
//         return;
//     }

//     printf("[BT] [INFO] %s:", path);
//     struct dirent *entry = nullptr;
//     int count = 0;
//     while ((entry = readdir(dir)) != nullptr) {
//         if (entry->d_name[0] == '.') {
//             continue;
//         }
//         printf(" %s", entry->d_name);
//         ++count;
//     }
//     closedir(dir);
//     printf(" (%d entr%s)\n", count, count == 1 ? "y" : "ies");
// }

// static void logPathState(const char *path) {
//     struct stat info = {};
//     if (stat(path, &info) == 0) {
//         printf("[BT] [INFO] %s: present\n", path);
//     } else {
//         printf("[BT] [INFO] %s: absent: %s\n", path, strerror(errno));
//     }
// }

static void logBluetoothKernelMessages() {
    int fd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        printf("[BT] [INFO] /dev/kmsg unavailable: %s\n", strerror(errno));
        return;
    }

    char buffer[1024];
    for (;;) {
        ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
        if (length <= 0) {
            break;
        }
        buffer[length] = '\0';
        if (strstr(buffer, "Bluetooth") || strstr(buffer, "bluetooth") ||
            strstr(buffer, "hci") || strstr(buffer, "BCM") || strstr(buffer, "firmware") ||
            strstr(buffer, "ttyAMA")) {
            printf("[BT] [KERNEL] %s", buffer);
        }
    }
    close(fd);
}

static int writeHciCommand(int fd, uint16_t opcode, const uint8_t *params, uint8_t paramLength) {
    uint8_t packet[32] = {};
    if (paramLength > sizeof(packet) - 4) {
        return RET_ERR;
    }
    packet[0] = AUDIOX_HCI_COMMAND_PKT;
    packet[1] = (uint8_t)(opcode & 0xFF);
    packet[2] = (uint8_t)(opcode >> 8);
    packet[3] = paramLength;
    if (params && paramLength > 0) {
        memcpy(packet + 4, params, paramLength);
    }
    ssize_t written = write(fd, packet, (size_t)paramLength + 4U);
    if (written != (ssize_t)paramLength + 4) {
        printf("[BT] [WARN] HCI command 0x%04x write failed: %s\n",
               (unsigned)opcode,
               strerror(errno));
    }
    return written == (ssize_t)paramLength + 4 ? RET_OK : RET_ERR;
}

static bool adapterIsUp(int fd, uint16_t deviceId) {
    hci::DeviceListRequest devices = {};
    devices.devNum = 16;
    if (ioctl(fd, AUDIOX_HCIGETDEVLIST, &devices) < 0) {
        return false;
    }
    for (uint16_t i = 0; i < devices.devNum && i < 16; ++i) {
        if (devices.devReq[i].devId == deviceId) {
            return (devices.devReq[i].devOpt & AUDIOX_HCI_DEV_UP) != 0;
        }
    }
    return false;
}

static bool waitForAdapterUp(int fd, uint16_t deviceId) {
    for (int attempt = 0; attempt < 100; ++attempt) {
        if (adapterIsUp(fd, deviceId)) {
            return true;
        }
        usleep(100000);
    }
    return false;
}

enum NameQuality : uint8_t {
    NAME_NONE = 0,
    NAME_SHORT = 1,
    NAME_COMPLETE = 2,
};

struct DiscoveredDevice {
    hci::Address address;
    char name[AUDIOX_BT_NAME_MAX];
    uint8_t nameQuality;
    bool classic;
    uint8_t pageScanRepMode;
    uint16_t clockOffset;
};

static void sanitizeName(const uint8_t *text, size_t length, char *out, size_t outSize) {
    size_t written = 0;
    for (size_t i = 0; i < length && written + 1 < outSize; ++i) {
        unsigned char value = text[i];
        if (value == 0) {
            break;
        }
        out[written++] = (value >= 0x20 && value != '"' && value != '\\') ? (char)value : ' ';
    }
    while (written > 0 && out[written - 1] == ' ') {
        --written;
    }
    out[written] = '\0';
}

static bool isZeroAddress(const hci::Address &address) {
    for (uint8_t byte : address.b) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

// Parses AD/EIR structures (shared by LE advertising and classic extended inquiry).
static uint8_t parseAdvertisingName(const uint8_t *data,
                                    size_t dataLength,
                                    char *name,
                                    size_t nameSize) {
    if (!data || !name || nameSize == 0) {
        return NAME_NONE;
    }
    name[0] = '\0';
    uint8_t quality = NAME_NONE;
    size_t offset = 0;
    while (offset < dataLength) {
        uint8_t fieldLength = data[offset++];
        if (fieldLength == 0 || offset + fieldLength > dataLength) {
            break;
        }
        uint8_t fieldType = data[offset];
        uint8_t fieldQuality = fieldType == 0x09 ? NAME_COMPLETE
                             : fieldType == 0x08 ? NAME_SHORT
                             : NAME_NONE;
        if (fieldQuality > quality) {
            sanitizeName(data + offset + 1, fieldLength - 1U, name, nameSize);
            if (name[0]) {
                quality = fieldQuality;
            }
        }
        offset += fieldLength;
    }
    return quality;
}

static DiscoveredDevice *addDiscoveredDevice(DiscoveredDevice *devices,
                                             size_t capacity,
                                             size_t *count,
                                             const hci::Address &address,
                                             const char *name,
                                             uint8_t quality) {
    if (!devices || !count || isZeroAddress(address)) {
        return nullptr;
    }
    DiscoveredDevice *device = nullptr;
    for (size_t i = 0; i < *count; ++i) {
        if (memcmp(devices[i].address.b, address.b, sizeof(address.b)) == 0) {
            device = &devices[i];
            break;
        }
    }
    if (!device) {
        if (*count >= capacity) {
            return nullptr;
        }
        device = &devices[(*count)++];
        memset(device, 0, sizeof(*device));
        device->address = address;
    }
    if (name && name[0] && quality > device->nameQuality) {
        snprintf(device->name, sizeof(device->name), "%s", name);
        device->nameQuality = quality;
    }
    return device;
}

static void addClassicDevice(DiscoveredDevice *devices,
                             size_t capacity,
                             size_t *count,
                             const uint8_t *addressBytes,
                             uint8_t pageScanRepMode,
                             uint16_t clockOffset,
                             const char *name,
                             uint8_t quality) {
    hci::Address address = {};
    memcpy(address.b, addressBytes, sizeof(address.b));
    DiscoveredDevice *device = addDiscoveredDevice(devices, capacity, count, address, name, quality);
    if (device) {
        device->classic = true;
        device->pageScanRepMode = pageScanRepMode;
        device->clockOffset = clockOffset;
    }
}

static int waitForCommand(int fd, uint16_t opcode, int timeoutMs) {
    struct pollfd pollFd = {fd, POLLIN, 0};
    uint8_t packet[64];
    const uint64_t deadline = monotonicMilliseconds() + (uint64_t)timeoutMs;
    while (monotonicMilliseconds() < deadline) {
        if (poll(&pollFd, 1, (int)(deadline - monotonicMilliseconds())) <= 0) {
            break;
        }
        ssize_t length = read(fd, packet, sizeof(packet));
        if (length < 7 || packet[0] != AUDIOX_HCI_EVENT_PKT) {
            continue;
        }
        if (packet[1] == AUDIOX_EVT_CMD_COMPLETE &&
            (uint16_t)(packet[4] | (packet[5] << 8)) == opcode) {
            return packet[6];
        }
        if (packet[1] == AUDIOX_EVT_CMD_STATUS &&
            (uint16_t)(packet[5] | (packet[6] << 8)) == opcode) {
            return packet[3];
        }
    }
    return -1;
}

static int scanLowEnergy(int adapterId,
                         DiscoveredDevice *devices,
                         size_t capacity,
                         size_t *count) {
    if (!devices || !count) {
        return RET_ERR;
    }

    int fd = socket(AUDIOX_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, AUDIOX_BTPROTO_HCI);
    if (fd < 0) {
        printf("[BT] [WARN] LE scan socket failed for hci%d: %s\n", adapterId, strerror(errno));
        return RET_ERR;
    }

    hci::SocketAddress address = {};
    address.family = AUDIOX_AF_BLUETOOTH;
    address.devId = (uint16_t)adapterId;
    address.channel = AUDIOX_HCI_CHANNEL_RAW;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        printf("[BT] [WARN] LE scan bind failed for hci%d: %s\n", adapterId, strerror(errno));
        close(fd);
        return RET_ERR;
    }
    if (configureEventFilter(fd) != RET_OK) {
        printf("[BT] [WARN] LE scan event filter failed: %s\n", strerror(errno));
        close(fd);
        return RET_ERR;
    }

    const uint8_t scanParams[] = {0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00};
    const uint8_t scanEnable[] = {0x01, 0x00};
    if (writeHciCommand(fd, AUDIOX_LE_SET_SCAN_PARAMS, scanParams, sizeof(scanParams)) != RET_OK ||
        writeHciCommand(fd, AUDIOX_LE_SET_SCAN_ENABLE, scanEnable, sizeof(scanEnable)) != RET_OK) {
        printf("[BT] [WARN] LE scan setup failed for hci%d\n", adapterId);
        close(fd);
        return RET_ERR;
    }

    printf("[BT] [INFO] LE scan started on hci%d\n", adapterId);
    struct pollfd pollFd = {fd, POLLIN, 0};
    uint8_t packet[512];
    int reportCount = 0;
    int pollResult = 0;
    const uint64_t deadline = monotonicMilliseconds() + 6000ULL;
    while (monotonicMilliseconds() < deadline) {
        uint64_t remaining = deadline - monotonicMilliseconds();
        int timeout = (remaining > 1000ULL) ? 1000 : (int)remaining;
        pollResult = poll(&pollFd, 1, timeout);
        if (pollResult <= 0) {
            break;
        }
        ssize_t length = read(fd, packet, sizeof(packet));
        if (length < 5 || packet[0] != AUDIOX_HCI_EVENT_PKT || packet[1] != AUDIOX_EVT_LE_META_EVENT) {
            continue;
        }

        size_t offset = 4;
        uint8_t reportCount = packet[offset++];
        bool extended = packet[3] == AUDIOX_LE_EXTENDED_ADVERTISING_REPORT;
        if (!extended && packet[3] != AUDIOX_LE_ADVERTISING_REPORT) {
            continue;
        }
        for (uint8_t report = 0; report < reportCount; ++report) {
            size_t addressOffset = extended ? offset + 3 : offset + 2;
            size_t dataLengthOffset = extended ? offset + 23 : offset + 8;
            if (dataLengthOffset >= (size_t)length || addressOffset + 6 > (size_t)length) {
                break;
            }
            hci::Address remote = {};
            memcpy(remote.b, packet + addressOffset, sizeof(remote.b));
            uint8_t dataLength = packet[dataLengthOffset];
            size_t dataOffset = dataLengthOffset + 1;
            if (dataOffset + dataLength > (size_t)length) {
                break;
            }
            offset = dataOffset;
            if (offset + dataLength > (size_t)length) {
                break;
            }
            char name[AUDIOX_BT_NAME_MAX] = {};
            uint8_t quality = parseAdvertisingName(packet + offset, dataLength, name, sizeof(name));
            // Legacy reports carry a trailing RSSI byte after the data.
            offset += dataLength + (extended ? 0U : 1U);
            addDiscoveredDevice(devices, capacity, count, remote, name, quality);
            ++reportCount;
        }
    }

    const uint8_t disable[] = {0x00, 0x00};
    (void)writeHciCommand(fd, AUDIOX_LE_SET_SCAN_ENABLE, disable, sizeof(disable));
        printf("[BT] [INFO] LE scan finished on hci%d: %d report(s), poll=%d%s\n",
            adapterId,
            reportCount,
            pollResult,
            pollResult < 0 ? strerror(errno) : "");
    close(fd);
    return RET_OK;
}

static int scanClassic(int fd,
                       DiscoveredDevice *devices,
                       size_t capacity,
                       size_t *count,
                       bool scan) {
    if (configureEventFilter(fd) != RET_OK) {
        printf("[BT] [WARN] classic scan event filter failed: %s\n", strerror(errno));
        return RET_ERR;
    }
    // Mode 2 enables Extended Inquiry Results, which carry the device name.
    const uint8_t inquiryMode[] = {0x02};
    if (writeHciCommand(fd, AUDIOX_HCI_WRITE_INQUIRY_MODE, inquiryMode, sizeof(inquiryMode)) != RET_OK ||
        waitForCommand(fd, AUDIOX_HCI_WRITE_INQUIRY_MODE, 500) != 0) {
        printf("[BT] [WARN] extended inquiry mode unavailable; names need remote lookup\n");
    }

    const uint8_t inquiryParams[] = {
        0x33, 0x8B, 0x9E,
        (uint8_t)(scan ? 6 : 1),
        0x00,
    };
    if (writeHciCommand(fd, AUDIOX_HCI_INQUIRY, inquiryParams, sizeof(inquiryParams)) != RET_OK) {
        return RET_ERR;
    }

    struct pollfd pollFd = {fd, POLLIN, 0};
    uint8_t packet[512];
    const uint64_t deadline = monotonicMilliseconds() + (scan ? 9000ULL : 3000ULL);
    while (monotonicMilliseconds() < deadline) {
        uint64_t remaining = deadline - monotonicMilliseconds();
        int timeout = (remaining > 1000ULL) ? 1000 : (int)remaining;
        if (poll(&pollFd, 1, timeout) <= 0) {
            continue;
        }
        ssize_t length = read(fd, packet, sizeof(packet));
        if (length < 4 || packet[0] != AUDIOX_HCI_EVENT_PKT) {
            continue;
        }
        uint8_t event = packet[1];
        if (event == AUDIOX_EVT_INQUIRY_COMPLETE) {
            break;
        }

        if (event == AUDIOX_EVT_EXTENDED_INQUIRY_RESULT && length >= 18) {
            char name[AUDIOX_BT_NAME_MAX] = {};
            uint8_t quality = parseAdvertisingName(packet + 18, (size_t)length - 18, name, sizeof(name));
            addClassicDevice(devices, capacity, count, packet + 4, packet[10],
                             (uint16_t)(packet[15] | (packet[16] << 8)), name, quality);
            continue;
        }

        if (event != AUDIOX_EVT_INQUIRY_RESULT && event != AUDIOX_EVT_INQUIRY_RESULT_RSSI) {
            continue;
        }
        uint8_t resultCount = packet[3];
        if (resultCount == 0) {
            continue;
        }
        size_t stride = ((size_t)length - 4) / resultCount;
        // Standard results and the pscan_mode RSSI variant put the clock offset at 12.
        size_t clockOffsetAt = (event == AUDIOX_EVT_INQUIRY_RESULT_RSSI && stride == 14) ? 11 : 12;
        if (stride < 14) {
            continue;
        }
        for (uint8_t i = 0; i < resultCount; ++i) {
            const uint8_t *entry = packet + 4 + (size_t)i * stride;
            addClassicDevice(devices, capacity, count, entry, entry[6],
                             (uint16_t)(entry[clockOffsetAt] | (entry[clockOffsetAt + 1] << 8)),
                             nullptr, NAME_NONE);
        }
    }
    return RET_OK;
}

// Classic devices that did not include a name in their inquiry response.
static void requestRemoteNames(int fd, DiscoveredDevice *devices, size_t count, int budgetMs) {
    struct pollfd pollFd = {fd, POLLIN, 0};
    uint8_t packet[300];
    const uint64_t deadline = monotonicMilliseconds() + (uint64_t)budgetMs;
    for (size_t i = 0; i < count && monotonicMilliseconds() < deadline; ++i) {
        DiscoveredDevice &device = devices[i];
        if (!device.classic || device.nameQuality == NAME_COMPLETE) {
            continue;
        }
        uint16_t clock = device.clockOffset | 0x8000;
        const uint8_t params[] = {
            device.address.b[0], device.address.b[1], device.address.b[2],
            device.address.b[3], device.address.b[4], device.address.b[5],
            device.pageScanRepMode, 0x00,
            (uint8_t)(clock & 0xFF), (uint8_t)(clock >> 8),
        };
        if (writeHciCommand(fd, AUDIOX_HCI_REMOTE_NAME_REQUEST, params, sizeof(params)) != RET_OK) {
            break;
        }

        bool done = false;
        while (!done && monotonicMilliseconds() < deadline) {
            uint64_t remaining = deadline - monotonicMilliseconds();
            int timeout = (remaining > 1000ULL) ? 1000 : (int)remaining;
            if (poll(&pollFd, 1, timeout) <= 0) {
                continue;
            }
            ssize_t length = read(fd, packet, sizeof(packet));
            if (length < 7 || packet[0] != AUDIOX_HCI_EVENT_PKT) {
                continue;
            }
            if (packet[1] == AUDIOX_EVT_CMD_STATUS &&
                (uint16_t)(packet[5] | (packet[6] << 8)) == AUDIOX_HCI_REMOTE_NAME_REQUEST &&
                packet[3] != 0) {
                done = true;
            } else if (packet[1] == AUDIOX_EVT_REMOTE_NAME_COMPLETE && length >= 10 &&
                       memcmp(packet + 4, device.address.b, sizeof(device.address.b)) == 0) {
                if (packet[3] == 0) {
                    char name[AUDIOX_BT_NAME_MAX] = {};
                    sanitizeName(packet + 10, (size_t)length - 10, name, sizeof(name));
                    if (name[0]) {
                        snprintf(device.name, sizeof(device.name), "%s", name);
                        device.nameQuality = NAME_COMPLETE;
                    }
                }
                done = true;
            }
        }

        if (!done) {
            // Free the controller before the LE scan starts.
            (void)writeHciCommand(fd, AUDIOX_HCI_REMOTE_NAME_CANCEL, params, 6);
            (void)waitForCommand(fd, AUDIOX_HCI_REMOTE_NAME_CANCEL, 500);
            break;
        }
    }
}

} // namespace

int bluetoothListDevices(char *out, size_t outSize, bool scan) {
    if (!out || outSize == 0) {
        return RET_ERR;
    }

    int socketFd = socket(AUDIOX_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, AUDIOX_BTPROTO_HCI);
    if (socketFd < 0) {
        printf("[BT] [ERR] HCI control socket failed: %s\n", strerror(errno));
        return RET_ERR;
    }

    DiscoveredDevice discovered[AUDIOX_BT_MAX_DEVICES] = {};
    size_t discoveredCount = 0;

    hci::DeviceListRequest devices = {};
    devices.devNum = 16;
    if (ioctl(socketFd, AUDIOX_HCIGETDEVLIST, &devices) < 0) {
        printf("[BT] [ERR] HCIGETDEVLIST failed: %s\n", strerror(errno));
        close(socketFd);
        return RET_ERR;
    }

    printf("[BT] [INFO] HCI reports %u adapter(s)\n", (unsigned)devices.devNum);
    for (uint16_t deviceIndex = 0; deviceIndex < devices.devNum && deviceIndex < 16; ++deviceIndex) {
        const hci::DeviceRequest &device = devices.devReq[deviceIndex];
        printf("[BT] [INFO] hci%u flags=0x%08x\n",
               (unsigned)device.devId,
               (unsigned)device.devOpt);
        if ((device.devOpt & AUDIOX_HCI_DEV_UP) == 0) {
            if ((device.devOpt & (AUDIOX_HCI_INIT | AUDIOX_HCI_SETUP)) != 0) {
                printf("[BT] [INFO] hci%u setup is already in progress; waiting\n",
                       (unsigned)device.devId);
                if (!waitForAdapterUp(socketFd, device.devId)) {
                    printf("[BT] [WARN] hci%u setup did not finish before scan timeout\n",
                           (unsigned)device.devId);
                    continue;
                }
            } else {
                int deviceId = (int)device.devId;
                if (ioctl(socketFd, AUDIOX_HCIDEVUP, deviceId) < 0) {
                    int savedErrno = errno;
                    if ((savedErrno == EINPROGRESS || savedErrno == EALREADY) &&
                        waitForAdapterUp(socketFd, device.devId)) {
                        printf("[BT] [INFO] hci%u finished asynchronous bring-up\n",
                               (unsigned)device.devId);
                    } else {
                        printf("[BT] [WARN] failed to bring hci%u up: %s\n",
                               (unsigned)device.devId,
                               strerror(savedErrno));
                        logBluetoothKernelMessages();
                        continue;
                    }
                } else {
                    printf("[BT] [INFO] brought hci%u up\n", (unsigned)device.devId);
                    if (!waitForAdapterUp(socketFd, device.devId)) {
                        printf("[BT] [WARN] hci%u did not finish setup before scanning\n",
                               (unsigned)device.devId);
                        continue;
                    }
                }
            }
        }

        hci::SocketAddress address = {};
        address.family = AUDIOX_AF_BLUETOOTH;
        address.devId = device.devId;
        int inquiryFd = socket(AUDIOX_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, AUDIOX_BTPROTO_HCI);
        if (inquiryFd < 0 || bind(inquiryFd, (struct sockaddr *)&address, sizeof(address)) < 0) {
            printf("[BT] [WARN] classic inquiry socket/bind failed for hci%u: %s\n",
                   (unsigned)device.devId,
                   strerror(errno));
            if (inquiryFd >= 0) {
                close(inquiryFd);
            }
        } else {
            int classicRc = scanClassic(inquiryFd,
                                        discovered,
                                        AUDIOX_BT_MAX_DEVICES,
                                        &discoveredCount,
                                        scan);
            printf("[BT] [INFO] classic inquiry completed on hci%u (rc=%d)\n",
                   (unsigned)device.devId,
                   classicRc);
            if (classicRc == RET_OK && scan) {
                requestRemoteNames(inquiryFd, discovered, discoveredCount, 4000);
            }
            close(inquiryFd);
        }

        if (scanLowEnergy((int)device.devId,
                          discovered,
                          AUDIOX_BT_MAX_DEVICES,
                          &discoveredCount) == RET_WARN) {
            break;
        }
    }
    close(socketFd);

    size_t used = 0;
    int n = snprintf(out, outSize, "{\"ok\":true,\"devices\":[");
    if (n < 0 || (size_t)n >= outSize) {
        return RET_ERR;
    }
    used = (size_t)n;
    for (size_t i = 0; i < discoveredCount; ++i) {
        char addressText[18] = {};
        formatAddress(discovered[i].address, addressText, sizeof(addressText));
        n = snprintf(out + used,
                     outSize - used,
                     "%s{\"address\":\"%s\",\"name\":\"%s\"}",
                     i > 0 ? "," : "",
                     addressText,
                     discovered[i].name);
        if (n < 0 || (size_t)n >= outSize - used) {
            return RET_ERR;
        }
        used += (size_t)n;
    }
    n = snprintf(out + used, outSize - used, "]}\n");
    if (n < 0 || (size_t)n >= outSize - used) {
        return RET_ERR;
    }
    return RET_OK;
}

int bluetoothPairDevice(const char *address) {
    (void)address;
    return RET_WARN;
}

int bluetoothConnectDevice(const char *address) {
    (void)address;
    return RET_WARN;
}