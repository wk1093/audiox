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

} // namespace hci

#define AUDIOX_HCI_DEV_UP 1U
#define AUDIOX_HCIDEVUP _IOW('H', 201, int)
#define AUDIOX_HCIGETDEVLIST _IOR('H', 210, int)
#define AUDIOX_HCIINQUIRY _IOR('H', 211, int)
#define AUDIOX_AF_BLUETOOTH 31
#define AUDIOX_BTPROTO_HCI 1
#define AUDIOX_HCI_COMMAND_PKT 0x01
#define AUDIOX_HCI_EVENT_PKT 0x04
#define AUDIOX_EVT_LE_META_EVENT 0x3E
#define AUDIOX_LE_ADVERTISING_REPORT 0x02
#define AUDIOX_HCI_CHANNEL_RAW 0
#define AUDIOX_LE_SET_SCAN_PARAMS 0x200B
#define AUDIOX_LE_SET_SCAN_ENABLE 0x200C

namespace {

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

static void logDirectoryEntries(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) {
        printf("[BT] [INFO] %s: unavailable: %s\n", path, strerror(errno));
        return;
    }

    printf("[BT] [INFO] %s:", path);
    struct dirent *entry = nullptr;
    int count = 0;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        printf(" %s", entry->d_name);
        ++count;
    }
    closedir(dir);
    printf(" (%d entr%s)\n", count, count == 1 ? "y" : "ies");
}

static void logPathState(const char *path) {
    struct stat info = {};
    if (stat(path, &info) == 0) {
        printf("[BT] [INFO] %s: present\n", path);
    } else {
        printf("[BT] [INFO] %s: absent: %s\n", path, strerror(errno));
    }
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

static bool appendDiscoveredDevice(char *out,
                                   size_t outSize,
                                   size_t *used,
                                   int *count,
                                   const hci::Address &address) {
    if (!out || !used || !count) {
        return false;
    }
    char addressText[18] = {};
    formatAddress(address, addressText, sizeof(addressText));
    int n = snprintf(out + *used,
                     outSize - *used,
                     "%s{\"address\":\"%s\",\"name\":\"\"}",
                     *count > 0 ? "," : "",
                     addressText);
    if (n < 0 || (size_t)n >= outSize - *used) {
        return false;
    }
    *used += (size_t)n;
    ++(*count);
    return true;
}

static int scanLowEnergy(int adapterId,
                         char *out,
                         size_t outSize,
                         size_t *used,
                         int *count) {
    if (!out || !used || !count) {
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
    while ((pollResult = poll(&pollFd, 1, 8000)) > 0) {
        ssize_t length = read(fd, packet, sizeof(packet));
        if (length < 7 || packet[0] != AUDIOX_HCI_EVENT_PKT || packet[1] != AUDIOX_EVT_LE_META_EVENT ||
            packet[3] != AUDIOX_LE_ADVERTISING_REPORT) {
            continue;
        }

        size_t offset = 4;
        uint8_t reportCount = packet[offset++];
        for (uint8_t report = 0; report < reportCount && offset + 10 <= (size_t)length; ++report) {
            offset += 2;
            hci::Address remote = {};
            memcpy(remote.b, packet + offset, sizeof(remote.b));
            offset += sizeof(remote.b);
            uint8_t dataLength = packet[offset++];
            if (offset + dataLength + 1 > (size_t)length) {
                break;
            }
            offset += dataLength + 1;
            if (!appendDiscoveredDevice(out, outSize, used, count, remote)) {
                close(fd);
                return RET_WARN;
            }
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

    size_t used = 0;
    int count = 0;
    int n = snprintf(out, outSize, "{\"ok\":true,\"devices\":[");
    if (n < 0 || (size_t)n >= outSize) {
        close(socketFd);
        return RET_ERR;
    }
    used = (size_t)n;

    hci::DeviceListRequest devices = {};
    devices.devNum = 16;
    if (ioctl(socketFd, AUDIOX_HCIGETDEVLIST, &devices) < 0) {
        printf("[BT] [ERR] HCIGETDEVLIST failed: %s\n", strerror(errno));
        close(socketFd);
        return RET_ERR;
    }

    printf("[BT] [INFO] HCI reports %u adapter(s)\n", (unsigned)devices.devNum);
    logDirectoryEntries("/sys/class/bluetooth");
    logDirectoryEntries("/sys/bus/serial/devices");
    logDirectoryEntries("/sys/class/tty");
    logPathState("/proc/device-tree/soc/serial@7e201000/bluetooth/status");
    logPathState("/dev/ttyAMA0");
    logPathState("/dev/ttyS0");
    logPathState("/sys/class/tty/ttyS0/device/of_node/compatible");
    logBluetoothKernelMessages();

    for (uint16_t deviceIndex = 0; deviceIndex < devices.devNum && deviceIndex < 16; ++deviceIndex) {
        const hci::DeviceRequest &device = devices.devReq[deviceIndex];
        printf("[BT] [INFO] hci%u flags=0x%08x\n",
               (unsigned)device.devId,
               (unsigned)device.devOpt);
        if ((device.devOpt & AUDIOX_HCI_DEV_UP) == 0) {
            int deviceId = (int)device.devId;
            if (ioctl(socketFd, AUDIOX_HCIDEVUP, deviceId) < 0) {
                printf("[BT] [WARN] failed to bring hci%u up: %s\n",
                       (unsigned)device.devId,
                       strerror(errno));
                logBluetoothKernelMessages();
                continue;
            }
            printf("[BT] [INFO] brought hci%u up\n", (unsigned)device.devId);
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
            hci::InquiryInfo responses[32] = {};
            hci::InquiryRequest inquiry = {};
            inquiry.devId = device.devId;
            inquiry.length = scan ? 8 : 1;
            inquiry.numResponses = 32;
            inquiry.responses = (uint8_t *)responses;
            if (ioctl(inquiryFd, AUDIOX_HCIINQUIRY, &inquiry) < 0) {
                printf("[BT] [WARN] classic inquiry failed on hci%u: %s\n",
                       (unsigned)device.devId,
                       strerror(errno));
            } else {
                printf("[BT] [INFO] classic inquiry found %u device(s) on hci%u\n",
                       (unsigned)inquiry.numResponses,
                       (unsigned)device.devId);
                for (uint8_t responseIndex = 0; responseIndex < inquiry.numResponses; ++responseIndex) {
                    if (!appendDiscoveredDevice(out, outSize, &used, &count, responses[responseIndex].address)) {
                        break;
                    }
                }
            }
            close(inquiryFd);
        }

        if (scanLowEnergy((int)device.devId, out, outSize, &used, &count) == RET_WARN) {
            break;
        }
    }
    close(socketFd);
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