#include "bluetooth/hci.hpp"

#include "defs.hpp"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace hci {

struct DeviceRequest {
    uint16_t devId;
    uint32_t devOpt;
};

struct DeviceListRequest {
    uint16_t devNum;
    DeviceRequest devReq[16];
};

struct SocketAddress {
    unsigned short family;
    uint16_t devId;
    uint16_t channel;
};

struct Filter {
    uint32_t typeMask;
    uint32_t eventMask[2];
    uint16_t opcode;
};

static_assert(sizeof(DeviceRequest) == 8, "unexpected HCI device request layout");
static_assert(offsetof(DeviceListRequest, devReq) == 4, "unexpected HCI device list layout");

} // namespace hci

#define AUDIOX_HCI_DEV_UP 1U
#define AUDIOX_HCI_INIT (1U << 16)
#define AUDIOX_HCI_SETUP (1U << 18)
#define AUDIOX_HCIDEVUP _IOW('H', 201, int)
#define AUDIOX_HCIGETDEVLIST _IOR('H', 210, int)
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
#define AUDIOX_HCI_WRITE_LOCAL_NAME 0x0C13
#define AUDIOX_EVT_LE_META_EVENT 0x3E
#define AUDIOX_LE_ADVERTISING_REPORT 0x02
#define AUDIOX_LE_EXTENDED_ADVERTISING_REPORT 0x0D
#define AUDIOX_HCI_CHANNEL_RAW 0
#define AUDIOX_SOL_HCI 0
#define AUDIOX_HCI_FILTER 2
#define AUDIOX_LE_SET_SCAN_PARAMS 0x200B
#define AUDIOX_LE_SET_SCAN_ENABLE 0x200C

namespace bt {

uint64_t monotonicMs() {
    struct timespec ts = {};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL;
}

void formatAddress(const Address &address, char *out, size_t outSize) {
    if (!out || outSize == 0) {
        return;
    }
    snprintf(out, outSize, "%02X:%02X:%02X:%02X:%02X:%02X",
             address.b[5], address.b[4], address.b[3],
             address.b[2], address.b[1], address.b[0]);
}

bool parseAddress(const char *text, Address *out) {
    if (!text || !out || strlen(text) != 17) {
        return false;
    }
    for (int i = 0; i < 6; ++i) {
        const char *part = text + i * 3;
        if (!isxdigit((unsigned char)part[0]) || !isxdigit((unsigned char)part[1]) ||
            (i < 5 && part[2] != ':')) {
            return false;
        }
        char hex[3] = {part[0], part[1], '\0'};
        out->b[5 - i] = (uint8_t)strtoul(hex, nullptr, 16);
    }
    return true;
}

bool isZeroAddress(const Address &address) {
    for (uint8_t byte : address.b) {
        if (byte != 0) {
            return false;
        }
    }
    return true;
}

bool sameAddress(const Address &a, const Address &b) {
    return memcmp(a.b, b.b, sizeof(a.b)) == 0;
}

static int configureEventFilter(int fd) {
    hci::Filter filter = {};
    filter.typeMask = 1U << AUDIOX_HCI_EVENT_PKT;
    filter.eventMask[0] = 0xFFFFFFFFU;
    filter.eventMask[1] = 0xFFFFFFFFU;
    return setsockopt(fd,
                      AUDIOX_SOL_HCI,
                      AUDIOX_HCI_FILTER,
                      &filter,
                      sizeof(filter)) == 0 ? RET_OK : RET_ERR;
}

int openRawSocket(int devId) {
    int fd = socket(AUDIOX_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, AUDIOX_BTPROTO_HCI);
    if (fd < 0) {
        printf("[BT] [WARN] HCI socket failed: %s\n", strerror(errno));
        return -1;
    }
    hci::SocketAddress address = {};
    address.family = AUDIOX_AF_BLUETOOTH;
    address.devId = (uint16_t)devId;
    address.channel = AUDIOX_HCI_CHANNEL_RAW;
    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0 ||
        configureEventFilter(fd) != RET_OK) {
        printf("[BT] [WARN] HCI socket setup failed for hci%d: %s\n", devId, strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

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

int writeCommand(int fd, uint16_t opcode, const uint8_t *params, uint8_t paramLength) {
    uint8_t packet[259] = {};
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

int setLocalName(int fd, const char *name) {
    if (!name || !name[0]) {
        return RET_ERR;
    }
    uint8_t params[248] = {};
    size_t length = strnlen(name, sizeof(params));
    if (length == sizeof(params)) {
        return RET_ERR;
    }
    memcpy(params, name, length);
    if (writeCommand(fd, AUDIOX_HCI_WRITE_LOCAL_NAME, params, sizeof(params)) != RET_OK) {
        return RET_ERR;
    }
    return waitForCommand(fd, AUDIOX_HCI_WRITE_LOCAL_NAME, 1000) == 0 ? RET_OK : RET_ERR;
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
                     : (fieldType == 0x08 || fieldType == 0x30) ? NAME_SHORT
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
                                             const Address &address,
                                             const char *name,
                                             uint8_t quality) {
    if (!devices || !count || isZeroAddress(address)) {
        return nullptr;
    }
    DiscoveredDevice *device = nullptr;
    for (size_t i = 0; i < *count; ++i) {
        if (sameAddress(devices[i].address, address)) {
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
    Address address = {};
    memcpy(address.b, addressBytes, sizeof(address.b));
    DiscoveredDevice *device = addDiscoveredDevice(devices, capacity, count, address, name, quality);
    if (device) {
        device->classic = true;
        device->pageScanRepMode = pageScanRepMode;
        device->clockOffset = clockOffset;
    }
}

int waitForCommand(int fd, uint16_t opcode, int timeoutMs) {
    struct pollfd pollFd = {fd, POLLIN, 0};
    uint8_t packet[64];
    const uint64_t deadline = monotonicMs() + (uint64_t)timeoutMs;
    while (monotonicMs() < deadline) {
        if (poll(&pollFd, 1, (int)(deadline - monotonicMs())) <= 0) {
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

static void scanLowEnergy(int fd,
                          DiscoveredDevice *devices,
                          size_t capacity,
                          size_t *count) {
    const uint8_t scanParams[] = {0x01, 0x10, 0x00, 0x10, 0x00, 0x00, 0x00};
    const uint8_t scanEnable[] = {0x01, 0x00};
    if (writeCommand(fd, AUDIOX_LE_SET_SCAN_PARAMS, scanParams, sizeof(scanParams)) != RET_OK ||
        waitForCommand(fd, AUDIOX_LE_SET_SCAN_PARAMS, 1000) != 0 ||
        writeCommand(fd, AUDIOX_LE_SET_SCAN_ENABLE, scanEnable, sizeof(scanEnable)) != RET_OK ||
        waitForCommand(fd, AUDIOX_LE_SET_SCAN_ENABLE, 1000) != 0) {
        printf("[BT] [WARN] LE scan setup failed\n");
        return;
    }

    struct pollfd pollFd = {fd, POLLIN, 0};
    uint8_t packet[512];
    int reportCount = 0;
    int scanResponseCount = 0;
    int namedReportCount = 0;
    int malformedReportCount = 0;
    const uint64_t deadline = monotonicMs() + 6000ULL;
    while (monotonicMs() < deadline) {
        uint64_t remaining = deadline - monotonicMs();
        int timeout = (remaining > 1000ULL) ? 1000 : (int)remaining;
        if (poll(&pollFd, 1, timeout) <= 0) {
            continue;
        }
        ssize_t length = read(fd, packet, sizeof(packet));
        if (length < 5 || packet[0] != AUDIOX_HCI_EVENT_PKT || packet[1] != AUDIOX_EVT_LE_META_EVENT) {
            continue;
        }

        size_t offset = 4;
        uint8_t batchCount = packet[offset++];
        bool extended = packet[3] == AUDIOX_LE_EXTENDED_ADVERTISING_REPORT;
        if (!extended && packet[3] != AUDIOX_LE_ADVERTISING_REPORT) {
            continue;
        }
        for (uint8_t report = 0; report < batchCount; ++report) {
            size_t addressOffset = extended ? offset + 3 : offset + 2;
            size_t dataLengthOffset = extended ? offset + 23 : offset + 8;
            if (dataLengthOffset >= (size_t)length || addressOffset + 6 > (size_t)length) {
                ++malformedReportCount;
                break;
            }
            uint16_t eventType = extended
                ? (uint16_t)(packet[offset] | ((uint16_t)packet[offset + 1] << 8))
                : packet[offset];
            if ((!extended && eventType == 0x04) ||
                (extended && (eventType & 0x0008U) != 0)) {
                ++scanResponseCount;
            }
            Address remote = {};
            memcpy(remote.b, packet + addressOffset, sizeof(remote.b));
            uint8_t dataLength = packet[dataLengthOffset];
            offset = dataLengthOffset + 1;
            if (offset + dataLength > (size_t)length) {
                ++malformedReportCount;
                break;
            }
            char name[BT_NAME_MAX] = {};
            uint8_t quality = parseAdvertisingName(packet + offset, dataLength, name, sizeof(name));
            if (quality != NAME_NONE) {
                ++namedReportCount;
            }
            // Legacy reports carry a trailing RSSI byte after the data.
            offset += dataLength + (extended ? 0U : 1U);
            addDiscoveredDevice(devices, capacity, count, remote, name, quality);
            ++reportCount;
        }
    }

    const uint8_t disable[] = {0x00, 0x00};
    (void)writeCommand(fd, AUDIOX_LE_SET_SCAN_ENABLE, disable, sizeof(disable));
    printf("[BT] [INFO] LE scan finished: reports=%d scan_responses=%d named_reports=%d malformed=%d\n",
           reportCount,
           scanResponseCount,
           namedReportCount,
           malformedReportCount);
}

static int scanClassic(int fd,
                       DiscoveredDevice *devices,
                       size_t capacity,
                       size_t *count) {
    // Mode 2 enables Extended Inquiry Results, which carry the device name.
    const uint8_t inquiryMode[] = {0x02};
    if (writeCommand(fd, AUDIOX_HCI_WRITE_INQUIRY_MODE, inquiryMode, sizeof(inquiryMode)) != RET_OK ||
        waitForCommand(fd, AUDIOX_HCI_WRITE_INQUIRY_MODE, 500) != 0) {
        printf("[BT] [WARN] extended inquiry mode unavailable; names need remote lookup\n");
    }

    const uint8_t inquiryParams[] = {0x33, 0x8B, 0x9E, 6, 0x00};
    if (writeCommand(fd, AUDIOX_HCI_INQUIRY, inquiryParams, sizeof(inquiryParams)) != RET_OK) {
        return RET_ERR;
    }

    struct pollfd pollFd = {fd, POLLIN, 0};
    uint8_t packet[512];
    const uint64_t deadline = monotonicMs() + 9000ULL;
    while (monotonicMs() < deadline) {
        uint64_t remaining = deadline - monotonicMs();
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
            char name[BT_NAME_MAX] = {};
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
    const uint64_t deadline = monotonicMs() + (uint64_t)budgetMs;
    for (size_t i = 0; i < count && monotonicMs() < deadline; ++i) {
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
        if (writeCommand(fd, AUDIOX_HCI_REMOTE_NAME_REQUEST, params, sizeof(params)) != RET_OK) {
            break;
        }

        bool done = false;
        while (!done && monotonicMs() < deadline) {
            uint64_t remaining = deadline - monotonicMs();
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
                    char name[BT_NAME_MAX] = {};
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
            (void)writeCommand(fd, AUDIOX_HCI_REMOTE_NAME_CANCEL, params, 6);
            (void)waitForCommand(fd, AUDIOX_HCI_REMOTE_NAME_CANCEL, 500);
            break;
        }
    }
}

int openAdapter() {
    int fd = socket(AUDIOX_AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, AUDIOX_BTPROTO_HCI);
    if (fd < 0) {
        printf("[BT] [ERR] HCI control socket failed: %s\n", strerror(errno));
        return -1;
    }

    hci::DeviceListRequest devices = {};
    devices.devNum = 16;
    if (ioctl(fd, AUDIOX_HCIGETDEVLIST, &devices) < 0 || devices.devNum == 0) {
        printf("[BT] [WARN] no HCI adapter available\n");
        close(fd);
        return -1;
    }

    const hci::DeviceRequest &device = devices.devReq[0];
    int devId = (int)device.devId;
    bool up = (device.devOpt & AUDIOX_HCI_DEV_UP) != 0;
    // The kernel is still running controller setup; HCIDEVUP would restart it.
    if (!up && (device.devOpt & (AUDIOX_HCI_INIT | AUDIOX_HCI_SETUP)) != 0) {
        up = waitForAdapterUp(fd, device.devId);
    } else if (!up) {
        if (ioctl(fd, AUDIOX_HCIDEVUP, devId) < 0 && errno != EALREADY && errno != EINPROGRESS) {
            printf("[BT] [WARN] failed to bring hci%d up: %s\n", devId, strerror(errno));
            logBluetoothKernelMessages();
        }
        up = waitForAdapterUp(fd, device.devId);
    }
    close(fd);

    if (!up) {
        printf("[BT] [WARN] hci%d did not come up\n", devId);
        return -1;
    }
    printf("[BT] [INFO] hci%d is up\n", devId);
    return devId;
}

size_t scan(int devId, DiscoveredDevice *out, size_t capacity) {
    if (!out || capacity == 0) {
        return 0;
    }
    int fd = openRawSocket(devId);
    if (fd < 0) {
        return 0;
    }

    size_t count = 0;
    if (scanClassic(fd, out, capacity, &count) == RET_OK) {
        requestRemoteNames(fd, out, count, 4000);
    }
    printf("[BT] [INFO] classic scan found %zu device(s)\n", count);
    scanLowEnergy(fd, out, capacity, &count);
    close(fd);
    return count;
}

} // namespace bt