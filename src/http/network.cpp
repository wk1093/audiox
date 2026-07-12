#include "http/network.hpp"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <errno.h>

bool isNetworkReady() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[INIT] [ERR] Failed to create socket for network interface check: %s\n", strerror(errno));
        return false;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, USB_NET_IFACE, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        close(fd);
        return false;
    }

    close(fd);
    return true;
}

void setupNetworkIp() {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[INIT] [ERR] Failed to create socket for network interface setup: %s\n", strerror(errno));
        return;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, USB_NET_IFACE, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, USB_NET_SERVER_IP, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to set IP address for usb0: %s\n", strerror(errno));
        close(fd);
        return;
    }

    inet_pton(AF_INET, USB_NET_NETMASK, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to set netmask for usb0: %s\n", strerror(errno));
        close(fd);
        return;
    }

    close(fd);
}

int setupNetworkInterface(Audiox *context) {
    for (int i = 0; i < 20; ++i) {
        if (isNetworkReady()) {
            setupNetworkIp();
            return RET_OK;
        }
        usleep(500 * 1000);
    }

    return RET_ERR;
}