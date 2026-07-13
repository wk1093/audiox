#include "http/network.hpp"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netpacket/packet.h>
#include <net/ethernet.h>

static inline int send_gratuitous_arp(const char *ifname, const char *ip_addr) {
    if (!ifname || !ifname[0] || !ip_addr || !ip_addr[0]) {
        return -1;
    }

    int ctl_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctl_fd < 0) {
        printf("[INIT] [WARN] Gratuitous ARP ctl socket failed: %s\n", strerror(errno));
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(ctl_fd, SIOCGIFINDEX, &ifr) < 0) {
        printf("[INIT] [WARN] Gratuitous ARP ifindex failed for %s: %s\n", ifname, strerror(errno));
        close(ctl_fd);
        return -1;
    }
    int ifindex = ifr.ifr_ifindex;

    if (ioctl(ctl_fd, SIOCGIFHWADDR, &ifr) < 0) {
        printf("[INIT] [WARN] Gratuitous ARP hwaddr failed for %s: %s\n", ifname, strerror(errno));
        close(ctl_fd);
        return -1;
    }
    uint8_t src_mac[6];
    memcpy(src_mac, ifr.ifr_hwaddr.sa_data, sizeof(src_mac));
    close(ctl_fd);

    struct in_addr src_ip;
    if (inet_pton(AF_INET, ip_addr, &src_ip) != 1) {
        printf("[INIT] [WARN] Gratuitous ARP invalid IP: %s\n", ip_addr);
        return -1;
    }

    int raw_fd = socket(AF_PACKET, SOCK_RAW, htons(ETH_P_ARP));
    if (raw_fd < 0) {
        printf("[INIT] [WARN] Gratuitous ARP raw socket failed: %s\n", strerror(errno));
        return -1;
    }

    uint8_t frame[42];
    memset(frame, 0, sizeof(frame));

    for (int i = 0; i < 6; ++i) {
        frame[i] = 0xff;
    }
    memcpy(&frame[6], src_mac, 6);
    frame[12] = 0x08;
    frame[13] = 0x06;

    frame[14] = 0x00;
    frame[15] = 0x01;
    frame[16] = 0x08;
    frame[17] = 0x00;
    frame[18] = 0x06;
    frame[19] = 0x04;
    frame[20] = 0x00;
    frame[21] = 0x01;
    memcpy(&frame[22], src_mac, 6);
    memcpy(&frame[28], &src_ip.s_addr, 4);
    memset(&frame[32], 0x00, 6);
    memcpy(&frame[38], &src_ip.s_addr, 4);

    struct sockaddr_ll dst;
    memset(&dst, 0, sizeof(dst));
    dst.sll_family = AF_PACKET;
    dst.sll_protocol = htons(ETH_P_ARP);
    dst.sll_ifindex = ifindex;
    dst.sll_halen = 6;
    for (int i = 0; i < 6; ++i) {
        dst.sll_addr[i] = 0xff;
    }

    ssize_t sent = sendto(raw_fd,
                          frame,
                          sizeof(frame),
                          0,
                          (struct sockaddr *)&dst,
                          sizeof(dst));
    if (sent < 0) {
        printf("[INIT] [WARN] Gratuitous ARP send failed on %s: %s\n", ifname, strerror(errno));
        close(raw_fd);
        return -1;
    }

    close(raw_fd);
    printf("[INIT] Gratuitous ARP announced %s on %s\n", ip_addr, ifname);
    return 0;
}


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

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to get flags for usb0: %s\n", strerror(errno));
        close(fd);
        return;
    }

    ifr.ifr_flags |= IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to set flags for usb0: %s\n", strerror(errno));
        close(fd);
        return;
    }

    for (int i = 0; i < 3; ++i) {
        (void)send_gratuitous_arp(USB_NET_IFACE, USB_NET_SERVER_IP);
        usleep(100 * 1000);
    }

    close(fd);
}

int setupNetworkInterface(Audiox *) {
    for (int i = 0; i < 20; ++i) {
        if (isNetworkReady()) {
            setupNetworkIp();
            return RET_OK;
        }
        usleep(500 * 1000);
    }

    return RET_ERR;
}