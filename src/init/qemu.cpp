// QEMU debug build support (make debug, AUDIOX_QEMU=1).
//
// The VM has no dwc2 controller, so the USB gadget binds to usbip-vudc, a
// virtual UDC that exports the gadget over USB/IP. This file brings up the
// virtio NIC (QEMU user networking) and serves the device side of the USB/IP
// handshake on TCP port AUDIOX_QEMU_USBIP_PORT, so the host can run
// `usbip attach -r 127.0.0.1 -b usbip-vudc.0` and see the AudioX USB device.
// Once a host imports the device the socket is handed to the kernel.
#include "init.hpp"

#if AUDIOX_QEMU

#include "system.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <net/if.h>
#include <net/route.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#ifndef AUDIOX_QEMU_NET_IFACE
#define AUDIOX_QEMU_NET_IFACE "eth0"
#endif
// QEMU user networking (slirp) defaults.
#ifndef AUDIOX_QEMU_NET_IP
#define AUDIOX_QEMU_NET_IP "10.0.2.15"
#endif
#ifndef AUDIOX_QEMU_NET_GATEWAY
#define AUDIOX_QEMU_NET_GATEWAY "10.0.2.2"
#endif
#ifndef AUDIOX_QEMU_USBIP_PORT
#define AUDIOX_QEMU_USBIP_PORT 3240
#endif

namespace {

constexpr uint16_t kUsbipVersion = 0x0111;
constexpr uint16_t kOpReqDevlist = 0x8005;
constexpr uint16_t kOpRepDevlist = 0x0005;
constexpr uint16_t kOpReqImport = 0x8003;
constexpr uint16_t kOpRepImport = 0x0003;
constexpr uint32_t kStatusOk = 0;
constexpr uint32_t kStatusError = 1;
constexpr size_t kBusIdSize = 32;
constexpr size_t kSysfsPathSize = 256;
constexpr size_t kUsbDeviceSize = kSysfsPathSize + kBusIdSize + 3 * 4 + 3 * 2 + 6;

const char kUdcName[] = AUDIOX_USB_GADGET_UDC;
const char kUdcClassDir[] = "/sys/class/udc/" AUDIOX_USB_GADGET_UDC;

int setInterface(int fd, const char *name, const char *ip, const char *mask) {
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, name, IFNAMSIZ - 1);
    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    addr->sin_family = AF_INET;

    inet_pton(AF_INET, ip, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        printf("[QEMU] [WARN] set address %s on %s failed: %s\n", ip, name, strerror(errno));
        return RET_WARN;
    }
    inet_pton(AF_INET, mask, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
        printf("[QEMU] [WARN] set netmask on %s failed: %s\n", name, strerror(errno));
        return RET_WARN;
    }
    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        printf("[QEMU] [WARN] get flags on %s failed: %s\n", name, strerror(errno));
        return RET_WARN;
    }
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        printf("[QEMU] [WARN] bring up %s failed: %s\n", name, strerror(errno));
        return RET_WARN;
    }
    return RET_OK;
}

int setupVmNetwork() {
    int fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        printf("[QEMU] [WARN] network socket failed: %s\n", strerror(errno));
        return RET_WARN;
    }
    (void)setInterface(fd, "lo", "127.0.0.1", "255.0.0.0");

    // virtio_net is loaded with the normal module list; give the device a moment to register.
    struct ifreq ifr;
    bool found = false;
    for (int i = 0; i < 50 && !found; ++i) {
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, AUDIOX_QEMU_NET_IFACE, IFNAMSIZ - 1);
        found = ioctl(fd, SIOCGIFFLAGS, &ifr) == 0;
        if (!found) {
            usleep(100 * 1000);
        }
    }
    if (!found) {
        printf("[QEMU] [WARN] %s not found; USB/IP and port forwards unavailable\n", AUDIOX_QEMU_NET_IFACE);
        close(fd);
        return RET_WARN;
    }

    int rc = setInterface(fd, AUDIOX_QEMU_NET_IFACE, AUDIOX_QEMU_NET_IP, "255.255.255.0");
    if (rc == RET_OK) {
        struct rtentry route;
        memset(&route, 0, sizeof(route));
        struct sockaddr_in *gw = (struct sockaddr_in *)&route.rt_gateway;
        gw->sin_family = AF_INET;
        inet_pton(AF_INET, AUDIOX_QEMU_NET_GATEWAY, &gw->sin_addr);
        ((struct sockaddr_in *)&route.rt_dst)->sin_family = AF_INET;
        ((struct sockaddr_in *)&route.rt_genmask)->sin_family = AF_INET;
        route.rt_flags = RTF_UP | RTF_GATEWAY;
        route.rt_dev = (char *)AUDIOX_QEMU_NET_IFACE;
        if (ioctl(fd, SIOCADDRT, &route) < 0 && errno != EEXIST) {
            printf("[QEMU] [WARN] default route via %s failed: %s\n", AUDIOX_QEMU_NET_GATEWAY, strerror(errno));
        }
        printf("[QEMU] %s up at %s\n", AUDIOX_QEMU_NET_IFACE, AUDIOX_QEMU_NET_IP);
    }
    close(fd);
    return rc;
}

ssize_t readSysfs(const char *dir, const char *attr, void *buf, size_t size) {
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/%s", dir, attr);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    ssize_t n = read(fd, buf, size);
    close(fd);
    return n;
}

int writeAll(int fd, const uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

int readAll(int fd, uint8_t *buf, size_t len) {
    while (len > 0) {
        ssize_t n = recv(fd, buf, len, 0);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return -1;
        }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

uint8_t *put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
    return p + 2;
}

uint8_t *put32(uint8_t *p, uint32_t v) {
    p = put16(p, (uint16_t)(v >> 16));
    return put16(p, (uint16_t)v);
}

int sendOpHeader(int fd, uint16_t code, uint32_t status) {
    uint8_t hdr[8];
    put32(put16(put16(hdr, kUsbipVersion), code), status);
    return writeAll(fd, hdr, sizeof(hdr));
}

uint32_t currentSpeed() {
    char text[32] = {};
    if (readSysfs(kUdcClassDir, "current_speed", text, sizeof(text) - 1) <= 0) {
        return 3;
    }
    if (strncmp(text, "low-speed", 9) == 0) return 1;
    if (strncmp(text, "full-speed", 10) == 0) return 2;
    // vudc is a high-speed controller; report that until the gadget is connected.
    return 3;
}

// Serialises struct usbip_usb_device (big-endian, packed) for the exported
// gadget. Fails when no gadget is bound, because vudc only exposes the device
// descriptor while a gadget driver is attached.
bool buildUsbDevice(uint8_t out[kUsbDeviceSize]) {
    char deviceDir[PATH_MAX];
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "%s/device", kUdcClassDir);
    if (!realpath(link, deviceDir)) {
        return false;
    }

    uint8_t desc[18];
    if (readSysfs(deviceDir, "dev_desc", desc, sizeof(desc)) != (ssize_t)sizeof(desc)) {
        return false;
    }

    memset(out, 0, kUsbDeviceSize);
    memcpy(out, deviceDir, strnlen(deviceDir, kSysfsPathSize - 1));
    memcpy(out + kSysfsPathSize, kUdcName, strnlen(kUdcName, kBusIdSize - 1));
    uint8_t *p = out + kSysfsPathSize + kBusIdSize;
    p = put32(p, 0); // busnum
    p = put32(p, 0); // devnum
    p = put32(p, currentSpeed());
    p = put16(p, (uint16_t)(desc[8] | (desc[9] << 8)));   // idVendor
    p = put16(p, (uint16_t)(desc[10] | (desc[11] << 8))); // idProduct
    p = put16(p, (uint16_t)(desc[12] | (desc[13] << 8))); // bcdDevice
    *p++ = desc[4];  // bDeviceClass
    *p++ = desc[5];  // bDeviceSubClass
    *p++ = desc[6];  // bDeviceProtocol
    *p++ = 0;        // bConfigurationValue
    *p++ = desc[17]; // bNumConfigurations
    *p++ = 0;        // bNumInterfaces (no interface records follow)
    return true;
}

void handleDevlist(int fd) {
    uint8_t device[kUsbDeviceSize];
    bool available = buildUsbDevice(device);
    uint8_t count[4];
    put32(count, available ? 1 : 0);
    if (sendOpHeader(fd, kOpRepDevlist, kStatusOk) < 0 || writeAll(fd, count, sizeof(count)) < 0) {
        return;
    }
    if (available) {
        (void)writeAll(fd, device, sizeof(device));
    }
}

void handleImport(int fd) {
    char busid[kBusIdSize + 1] = {};
    if (readAll(fd, (uint8_t *)busid, kBusIdSize) < 0) {
        return;
    }

    uint8_t device[kUsbDeviceSize];
    if (strcmp(busid, kUdcName) != 0 || !buildUsbDevice(device)) {
        printf("[QEMU] [WARN] USB/IP import of '%s' refused: no bound gadget with that bus id\n", busid);
        (void)sendOpHeader(fd, kOpRepImport, kStatusError);
        return;
    }

    char deviceDir[PATH_MAX];
    char link[PATH_MAX];
    snprintf(link, sizeof(link), "%s/device", kUdcClassDir);
    char sockfdPath[PATH_MAX];
    char value[16];
    snprintf(value, sizeof(value), "%d\n", fd);
    if (!realpath(link, deviceDir) ||
        snprintf(sockfdPath, sizeof(sockfdPath), "%s/usbip_sockfd", deviceDir) >= (int)sizeof(sockfdPath) ||
        writeSysNode(sockfdPath, value) != RET_OK) {
        printf("[QEMU] [WARN] USB/IP import failed: device busy or gadget not connected (%s)\n", strerror(errno));
        (void)sendOpHeader(fd, kOpRepImport, kStatusError);
        return;
    }

    // The kernel now holds its own reference to the socket and drives URB traffic.
    if (sendOpHeader(fd, kOpRepImport, kStatusOk) == 0) {
        (void)writeAll(fd, device, sizeof(device));
    }
    printf("[QEMU] USB/IP: host imported %s\n", kUdcName);
}

void *usbipExporterThread(void *) {
    int listenFd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listenFd < 0) {
        printf("[QEMU] [WARN] USB/IP socket failed: %s\n", strerror(errno));
        return nullptr;
    }
    int one = 1;
    (void)setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(AUDIOX_QEMU_USBIP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(listenFd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(listenFd, 4) < 0) {
        printf("[QEMU] [WARN] USB/IP listen on port %d failed: %s\n", AUDIOX_QEMU_USBIP_PORT, strerror(errno));
        close(listenFd);
        return nullptr;
    }
    printf("[QEMU] USB/IP exporter listening on port %d (bus id %s)\n", AUDIOX_QEMU_USBIP_PORT, kUdcName);

    while (true) {
        int fd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno != EINTR) {
                usleep(100 * 1000);
            }
            continue;
        }
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        (void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

        uint8_t hdr[8];
        if (readAll(fd, hdr, sizeof(hdr)) == 0) {
            uint16_t version = (uint16_t)((hdr[0] << 8) | hdr[1]);
            uint16_t code = (uint16_t)((hdr[2] << 8) | hdr[3]);
            if (version != kUsbipVersion) {
                printf("[QEMU] [WARN] USB/IP client version 0x%04x unsupported\n", version);
            } else if (code == kOpReqDevlist) {
                handleDevlist(fd);
            } else if (code == kOpReqImport) {
                handleImport(fd);
            } else {
                printf("[QEMU] [WARN] USB/IP request 0x%04x unsupported\n", code);
            }
        }
        close(fd);
    }
    return nullptr;
}

} // namespace

int setupQemuDebug(void *) {
    printf("[QEMU] debug build: storage %s/%s, UDC %s\n", BOOT_DEVICE_PATH, CONFIG_DEVICE_PATH, kUdcName);
    int rc = setupVmNetwork();

    pthread_t thread;
    if (pthread_create(&thread, nullptr, usbipExporterThread, nullptr) != 0) {
        printf("[QEMU] [WARN] failed to start USB/IP exporter thread\n");
        return RET_WARN;
    }
    pthread_detach(thread);
    return rc;
}

#endif
