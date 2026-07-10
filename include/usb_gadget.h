#ifndef USB_GADGET_H
#define USB_GADGET_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <net/if.h>
#include <net/ethernet.h>
#include <net/if_arp.h>
#include <netpacket/packet.h>
#include <arpa/inet.h>
#include <netinet/in.h>

#include "init_defs.h"
#include "init_helpers.h"

typedef struct dhcp_server {
    int fd;
    uint32_t server_ip;
    uint32_t lease_ip;
    uint32_t netmask;
    int lease_active;
    uint8_t lease_mac[6];
} dhcp_server_t;

static dhcp_server_t g_dhcp_prebind = { .fd = -1 };
static int g_dhcp_prebind_ready = 0;

#define DHCP_SERVER_PORT 67
#define DHCP_CLIENT_PORT 68
#define DHCP_MAGIC_COOKIE 0x63825363u
#define DHCP_OPT_MSG_TYPE 53
#define DHCP_OPT_REQ_IP 50
#define DHCP_OPT_SERVER_ID 54
#define DHCP_OPT_SUBNET_MASK 1
#define DHCP_OPT_ROUTER 3
#define DHCP_OPT_LEASE_TIME 51
#define DHCP_OPT_END 255
#define DHCP_MSG_DISCOVER 1
#define DHCP_MSG_OFFER 2
#define DHCP_MSG_REQUEST 3
#define DHCP_MSG_ACK 5
#define DHCP_MSG_NAK 6
#define DHCP_DISCOVER_WINDOW_MS 120000
#define DHCP_POST_DISCOVER_MS 5000
#define USB_NET_IFACE "usb0"
#define USB_NET_SERVER_IP "169.254.1.2"
#define USB_NET_NETMASK "255.255.0.0"

typedef struct dhcp_packet {
    uint8_t op;
    uint8_t htype;
    uint8_t hlen;
    uint8_t hops;
    uint32_t xid;
    uint16_t secs;
    uint16_t flags;
    uint32_t ciaddr;
    uint32_t yiaddr;
    uint32_t siaddr;
    uint32_t giaddr;
    uint8_t chaddr[16];
    uint8_t sname[64];
    uint8_t file[128];
    uint32_t cookie;
    uint8_t options[312];
} __attribute__((packed)) dhcp_packet_t;

static inline uint32_t dhcp_u32_be(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) |
           ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) |
           ((uint32_t)p[3]);
}

static inline uint16_t dhcp_u16_be(const uint8_t *p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
}

static inline int dhcp_get_msg_type(const dhcp_packet_t *pkt, size_t len) {
    if (len < sizeof(dhcp_packet_t) - sizeof(pkt->options)) {
        return 0;
    }
    if (ntohl(pkt->cookie) != DHCP_MAGIC_COOKIE) {
        return 0;
    }

    size_t base = sizeof(dhcp_packet_t) - sizeof(pkt->options);
    size_t opt_len = len > base ? (len - base) : 0;
    size_t i = 0;
    while (i < opt_len) {
        uint8_t code = pkt->options[i++];
        if (code == DHCP_OPT_END) {
            break;
        }
        if (code == 0) {
            continue;
        }
        if (i >= opt_len) {
            break;
        }
        uint8_t olen = pkt->options[i++];
        if ((size_t)olen > opt_len - i) {
            break;
        }
        if (code == DHCP_OPT_MSG_TYPE && olen == 1) {
            return pkt->options[i];
        }
        i += olen;
    }

    return 0;
}

static inline uint32_t dhcp_get_requested_ip(const dhcp_packet_t *pkt, size_t len) {
    if (len < sizeof(dhcp_packet_t) - sizeof(pkt->options)) {
        return 0;
    }
    if (ntohl(pkt->cookie) != DHCP_MAGIC_COOKIE) {
        return 0;
    }

    size_t base = sizeof(dhcp_packet_t) - sizeof(pkt->options);
    size_t opt_len = len > base ? (len - base) : 0;
    size_t i = 0;
    while (i < opt_len) {
        uint8_t code = pkt->options[i++];
        if (code == DHCP_OPT_END) {
            break;
        }
        if (code == 0) {
            continue;
        }
        if (i >= opt_len) {
            break;
        }
        uint8_t olen = pkt->options[i++];
        if ((size_t)olen > opt_len - i) {
            break;
        }
        if (code == DHCP_OPT_REQ_IP && olen == 4) {
            return dhcp_u32_be(&pkt->options[i]);
        }
        i += olen;
    }

    return 0;
}

static inline int dhcp_mac_equal(const uint8_t *a, const uint8_t *b) {
    for (int i = 0; i < 6; ++i) {
        if (a[i] != b[i]) {
            return 0;
        }
    }
    return 1;
}

static inline size_t dhcp_put_opt_u32(uint8_t *dst, size_t pos, size_t cap, uint8_t code, uint32_t value) {
    if (pos + 6 > cap) {
        return pos;
    }
    dst[pos++] = code;
    dst[pos++] = 4;
    dst[pos++] = (uint8_t)((value >> 24) & 0xff);
    dst[pos++] = (uint8_t)((value >> 16) & 0xff);
    dst[pos++] = (uint8_t)((value >> 8) & 0xff);
    dst[pos++] = (uint8_t)(value & 0xff);
    return pos;
}

static inline int dhcp_send_reply(dhcp_server_t *srv,
                                  const dhcp_packet_t *req,
                                  int msg_type,
                                  uint32_t yiaddr_be) {
    dhcp_packet_t resp;
    memset(&resp, 0, sizeof(resp));

    resp.op = 2;
    resp.htype = 1;
    resp.hlen = 6;
    resp.xid = req->xid;
    resp.flags = req->flags;
    resp.yiaddr = yiaddr_be;
    resp.siaddr = srv->server_ip;
    memcpy(resp.chaddr, req->chaddr, 16);
    resp.cookie = htonl(DHCP_MAGIC_COOKIE);

    size_t pos = 0;
    if (pos + 3 >= sizeof(resp.options)) {
        return -1;
    }
    resp.options[pos++] = DHCP_OPT_MSG_TYPE;
    resp.options[pos++] = 1;
    resp.options[pos++] = (uint8_t)msg_type;

    pos = dhcp_put_opt_u32(resp.options, pos, sizeof(resp.options), DHCP_OPT_SERVER_ID, ntohl(srv->server_ip));
    if (msg_type == DHCP_MSG_OFFER || msg_type == DHCP_MSG_ACK) {
        pos = dhcp_put_opt_u32(resp.options, pos, sizeof(resp.options), DHCP_OPT_SUBNET_MASK, ntohl(srv->netmask));
        pos = dhcp_put_opt_u32(resp.options, pos, sizeof(resp.options), DHCP_OPT_ROUTER, ntohl(srv->server_ip));
        pos = dhcp_put_opt_u32(resp.options, pos, sizeof(resp.options), DHCP_OPT_LEASE_TIME, 3600);
    }

    if (pos >= sizeof(resp.options)) {
        return -1;
    }
    resp.options[pos++] = DHCP_OPT_END;

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = htons(DHCP_CLIENT_PORT);
    dst.sin_addr.s_addr = INADDR_BROADCAST;

    ssize_t sent = sendto(srv->fd,
                          &resp,
                          sizeof(resp),
                          0,
                          (struct sockaddr *)&dst,
                          sizeof(dst));
    if (sent < 0) {
        printf("[INIT] [ERR] DHCP send failed: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}

static inline int dhcp_server_open(dhcp_server_t *srv,
                                   const char *ifname,
                                   const char *server_ip,
                                   const char *lease_ip,
                                   const char *netmask) {
    if (!srv || !server_ip || !lease_ip || !netmask) {
        return -1;
    }

    memset(srv, 0, sizeof(*srv));
    srv->fd = -1;

    if (inet_pton(AF_INET, server_ip, &srv->server_ip) != 1) {
        printf("[INIT] [ERR] DHCP invalid server IP: %s\n", server_ip);
        return -1;
    }
    if (inet_pton(AF_INET, lease_ip, &srv->lease_ip) != 1) {
        printf("[INIT] [ERR] DHCP invalid lease IP: %s\n", lease_ip);
        return -1;
    }
    if (inet_pton(AF_INET, netmask, &srv->netmask) != 1) {
        printf("[INIT] [ERR] DHCP invalid netmask: %s\n", netmask);
        return -1;
    }

    srv->fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (srv->fd < 0) {
        printf("[INIT] [ERR] DHCP socket failed: %s\n", strerror(errno));
        return -1;
    }

    int yes = 1;
    (void)setsockopt(srv->fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    (void)setsockopt(srv->fd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
#ifdef SO_BINDTODEVICE
    if (ifname && ifname[0]) {
        if (setsockopt(srv->fd, SOL_SOCKET, SO_BINDTODEVICE, ifname, strlen(ifname) + 1) < 0) {
            printf("[INIT] [WARN] DHCP SO_BINDTODEVICE(%s) failed: %s\n", ifname, strerror(errno));
        }
    }
#endif

    int flags = fcntl(srv->fd, F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(srv->fd, F_SETFL, flags | O_NONBLOCK);
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(DHCP_SERVER_PORT);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(srv->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[INIT] [ERR] DHCP bind failed: %s\n", strerror(errno));
        close(srv->fd);
        srv->fd = -1;
        return -1;
    }

    printf("[INIT] DHCP ready on %s (offer %s)\n", (ifname && ifname[0]) ? ifname : "any", lease_ip);
    return 0;
}

static inline int prepare_network_dhcp_server(void) {
    if (g_dhcp_prebind_ready) {
        return 0;
    }

    if (dhcp_server_open(&g_dhcp_prebind,
                         NULL,
                         USB_NET_SERVER_IP,
                         "169.254.1.1",
                         USB_NET_NETMASK) < 0) {
        return -1;
    }

    g_dhcp_prebind_ready = 1;
    printf("[INIT] DHCP prebind ready before UDC bind.\n");
    return 0;
}

static inline void dhcp_server_close(dhcp_server_t *srv) {
    if (!srv) {
        return;
    }
    if (srv->fd >= 0) {
        close(srv->fd);
        srv->fd = -1;
    }
}

static inline int dhcp_server_poll(dhcp_server_t *srv) {
    if (!srv || srv->fd < 0) {
        return -1;
    }

    dhcp_packet_t req;
    struct sockaddr_in src;
    socklen_t src_len = sizeof(src);

    ssize_t n = recvfrom(srv->fd,
                         &req,
                         sizeof(req),
                         0,
                         (struct sockaddr *)&src,
                         &src_len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return 0;
        }
        printf("[INIT] [ERR] DHCP recv failed: %s\n", strerror(errno));
        return -1;
    }

    if (n < (ssize_t)(sizeof(dhcp_packet_t) - sizeof(req.options))) {
        return 0;
    }
    if (req.op != 1 || req.htype != 1 || req.hlen < 6) {
        return 0;
    }

    int msg_type = dhcp_get_msg_type(&req, (size_t)n);
    if (msg_type == DHCP_MSG_DISCOVER) {
        printf("[INIT] DHCP discover from %02x:%02x:%02x:%02x:%02x:%02x\n",
               req.chaddr[0], req.chaddr[1], req.chaddr[2],
               req.chaddr[3], req.chaddr[4], req.chaddr[5]);
        (void)dhcp_send_reply(srv, &req, DHCP_MSG_OFFER, srv->lease_ip);
        return 1;
    }

    if (msg_type == DHCP_MSG_REQUEST) {
        uint32_t requested_ip = dhcp_get_requested_ip(&req, (size_t)n);
        uint32_t ciaddr = ntohl(req.ciaddr);
        uint32_t offered = ntohl(srv->lease_ip);

        printf("[INIT] DHCP request from %02x:%02x:%02x:%02x:%02x:%02x req=%u.%u.%u.%u ci=%u.%u.%u.%u\n",
               req.chaddr[0], req.chaddr[1], req.chaddr[2],
               req.chaddr[3], req.chaddr[4], req.chaddr[5],
               (unsigned)((requested_ip >> 24) & 0xff),
               (unsigned)((requested_ip >> 16) & 0xff),
               (unsigned)((requested_ip >> 8) & 0xff),
               (unsigned)(requested_ip & 0xff),
               (unsigned)((ciaddr >> 24) & 0xff),
               (unsigned)((ciaddr >> 16) & 0xff),
               (unsigned)((ciaddr >> 8) & 0xff),
               (unsigned)(ciaddr & 0xff));

        // Be permissive: some clients include stale/missing req-ip and rely on server-id matching.
        if (requested_ip != 0 && requested_ip != offered) {
            printf("[INIT] [WARN] DHCP request asked for different IP, ACKing offered lease anyway.\n");
        }
        if (requested_ip == 0 && ciaddr != 0 && ciaddr != offered) {
            printf("[INIT] [WARN] DHCP request ciaddr differs, ACKing offered lease anyway.\n");
        }

        if (!srv->lease_active || dhcp_mac_equal(srv->lease_mac, req.chaddr)) {
            memcpy(srv->lease_mac, req.chaddr, 6);
            srv->lease_active = 1;
            (void)dhcp_send_reply(srv, &req, DHCP_MSG_ACK, srv->lease_ip);
            printf("[INIT] DHCP lease -> %u.%u.%u.%u\n",
                   (unsigned)((offered >> 24) & 0xff),
                   (unsigned)((offered >> 16) & 0xff),
                   (unsigned)((offered >> 8) & 0xff),
                   (unsigned)(offered & 0xff));
            return 2;
        }

        printf("[INIT] [WARN] DHCP request from second MAC ignored during single-lease window.\n");
        return 0;
    }

    return 1;
}

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

static inline int setup_usb_audio_gadget_layout(unsigned int playback_channels,
                                                unsigned int capture_channels,
                                                unsigned int sample_rate,
                                                unsigned int sample_size_bytes);

static inline int usb_audio_gadget_reconfigure_layout(unsigned int playback_channels,
                                                      unsigned int capture_channels,
                                                      unsigned int sample_rate,
                                                      unsigned int sample_size_bytes);

static inline int setup_usb_audio_gadget(void) {
    return setup_usb_audio_gadget_layout(2, 2, SAMPLE_RATE, 2);
}

static inline uint32_t usb_audio_channel_mask(unsigned int channels) {
    if (channels == 0 || channels > 16) {
        return 0;
    }
    return (1u << channels) - 1u;
}

static inline int write_sys_node_u32(const char *path, uint32_t value) {
    char buf[32];
    int n = snprintf(buf, sizeof(buf), "%u\n", (unsigned)value);
    if (n <= 0 || (size_t)n >= sizeof(buf)) {
        return -1;
    }
    return write_sys_node(path, buf);
}

static inline int read_sys_node_u32(const char *path, uint32_t *value_out) {
    if (!path || !value_out) {
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    char buf[64];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) {
        return -1;
    }

    buf[n] = '\0';
    char *endp = NULL;
    unsigned long v = strtoul(buf, &endp, 10);
    if (endp == buf) {
        return -1;
    }

    *value_out = (uint32_t)v;
    return 0;
}

static inline unsigned int usb_audio_channels_from_mask(uint32_t mask) {
    if (mask == 0) {
        return 0;
    }

    unsigned int channels = 0;
    uint32_t m = mask;
    while ((m & 1u) != 0u) {
        ++channels;
        m >>= 1;
    }

    if (m != 0u || channels > 16U) {
        return 0;
    }
    return channels;
}

static inline int usb_audio_bind_udc_retry(void) {
    for (int i = 0; i < 8; ++i) {
        if (write_sys_node(GADGET_UDC_NODE, GADGET_UDC_NAME) == 0) {
            return 0;
        }
        usleep(30000);
    }

    printf("[INIT] [ERR] USB gadget bind failed after retries (UDC=%s, name=%s).\n",
           GADGET_UDC_NODE,
           GADGET_UDC_NAME);
    return -1;
}

static inline void usb_audio_unlink_config_link(void) {
    if (unlink(GADGET_CONFIG_LINK) == 0) {
        printf("[INIT] Unlinked existing UAC2 config link before layout update.\n");
        return;
    }

    if (errno == ENOENT) {
        return;
    }

    printf("[INIT] [WARN] Failed to unlink UAC2 config link (%s): %s\n",
           GADGET_CONFIG_LINK,
           strerror(errno));
}

static inline int setup_usb_audio_gadget_layout(unsigned int playback_channels,
                                                unsigned int capture_channels,
                                                unsigned int sample_rate,
                                                unsigned int sample_size_bytes) {
    uint32_t p_chmask = usb_audio_channel_mask(playback_channels);
    uint32_t c_chmask = usb_audio_channel_mask(capture_channels);

    if (p_chmask == 0 || c_chmask == 0) {
        printf("[INIT] [ERR] Invalid USB audio channel layout p=%u c=%u\n",
               playback_channels,
               capture_channels);
        return -1;
    }

    if (sample_rate < 8000 || sample_rate > 192000) {
        printf("[INIT] [ERR] Invalid USB audio sample rate: %u\n", sample_rate);
        return -1;
    }

    if (sample_size_bytes == 0 || sample_size_bytes > 4) {
        printf("[INIT] [ERR] Invalid USB audio sample size: %u\n", sample_size_bytes);
        return -1;
    }

    if (ensure_dir(GADGET_ROOT, 0755) < 0) return -1;

    if (write_sys_node(GADGET_ROOT "/idVendor", "0x6666\n") < 0) return -1;
    if (write_sys_node(GADGET_ROOT "/idProduct", "0x0169\n") < 0) return -1;
    if (write_sys_node(GADGET_ROOT "/bcdDevice", "0x0100\n") < 0) return -1;
    if (write_sys_node(GADGET_ROOT "/bcdUSB", "0x0200\n") < 0) return -1;

    if (ensure_dir(GADGET_ROOT "/strings/0x409", 0755) < 0) return -1;
    if (write_sys_node(GADGET_ROOT "/strings/0x409/serialnumber", "0000000001\n") < 0) return -1;
    if (write_sys_node(GADGET_ROOT "/strings/0x409/manufacturer", "WyattKloos\n") < 0) return -1;
    if (write_sys_node(GADGET_ROOT "/strings/0x409/product", "audiox\n") < 0) return -1;

    if (ensure_dir(GADGET_ROOT "/configs/c.1", 0755) < 0) return -1;
    if (ensure_dir(GADGET_ROOT "/configs/c.1/strings/0x409", 0755) < 0) return -1;
    if (write_sys_node(GADGET_ROOT "/configs/c.1/strings/0x409/configuration", "UAC2 Audio Stream\n") < 0) return -1;

    if (ensure_dir(GADGET_UAC2_FUNC, 0755) < 0) return -1;

    if (write_sys_node_u32(GADGET_UAC2_FUNC "/c_chmask", c_chmask) < 0) return -1;
    if (write_sys_node_u32(GADGET_UAC2_FUNC "/c_srate", sample_rate) < 0) return -1;
    if (write_sys_node_u32(GADGET_UAC2_FUNC "/c_ssize", sample_size_bytes) < 0) return -1;

    if (write_sys_node_u32(GADGET_UAC2_FUNC "/p_chmask", p_chmask) < 0) return -1;
    if (write_sys_node_u32(GADGET_UAC2_FUNC "/p_srate", sample_rate) < 0) return -1;
    if (write_sys_node_u32(GADGET_UAC2_FUNC "/p_ssize", sample_size_bytes) < 0) return -1;

    if (symlink(GADGET_UAC2_FUNC, GADGET_CONFIG_LINK) < 0 && errno != EEXIST) {
        printf("[INIT] [ERR] Symlink layout assignment failed: %s\n", strerror(errno));
        return -1;
    }

    printf("[INIT] USB audio layout applied: playback=%uch capture=%uch rate=%u ssize=%u\n",
           playback_channels,
           capture_channels,
           sample_rate,
           sample_size_bytes);

    return 0;
}

static inline int usb_audio_gadget_reconfigure_layout(unsigned int playback_channels,
                                                      unsigned int capture_channels,
                                                      unsigned int sample_rate,
                                                      unsigned int sample_size_bytes) {
    unsigned int prev_playback_channels = 2;
    unsigned int prev_capture_channels = 2;
    unsigned int prev_sample_rate = SAMPLE_RATE;
    unsigned int prev_sample_size = 2;

    uint32_t prev_c_chmask = 0;
    uint32_t prev_p_chmask = 0;
    uint32_t prev_c_srate = 0;
    uint32_t prev_p_srate = 0;
    uint32_t prev_c_ssize = 0;
    uint32_t prev_p_ssize = 0;

    if (read_sys_node_u32(GADGET_UAC2_FUNC "/c_chmask", &prev_c_chmask) == 0) {
        unsigned int ch = usb_audio_channels_from_mask(prev_c_chmask);
        if (ch > 0) {
            prev_capture_channels = ch;
        }
    }
    if (read_sys_node_u32(GADGET_UAC2_FUNC "/p_chmask", &prev_p_chmask) == 0) {
        unsigned int ch = usb_audio_channels_from_mask(prev_p_chmask);
        if (ch > 0) {
            prev_playback_channels = ch;
        }
    }
    if (read_sys_node_u32(GADGET_UAC2_FUNC "/c_srate", &prev_c_srate) == 0 && prev_c_srate > 0) {
        prev_sample_rate = (unsigned int)prev_c_srate;
    } else if (read_sys_node_u32(GADGET_UAC2_FUNC "/p_srate", &prev_p_srate) == 0 && prev_p_srate > 0) {
        prev_sample_rate = (unsigned int)prev_p_srate;
    }
    if (read_sys_node_u32(GADGET_UAC2_FUNC "/c_ssize", &prev_c_ssize) == 0 && prev_c_ssize > 0) {
        prev_sample_size = (unsigned int)prev_c_ssize;
    } else if (read_sys_node_u32(GADGET_UAC2_FUNC "/p_ssize", &prev_p_ssize) == 0 && prev_p_ssize > 0) {
        prev_sample_size = (unsigned int)prev_p_ssize;
    }

    int unbound = 0;
    if (write_sys_node(GADGET_UDC_NODE, "\n") == 0) {
        unbound = 1;
    } else if (write_sys_node(GADGET_UDC_NODE, "") == 0) {
        unbound = 1;
    }

    if (!unbound) {
        printf("[INIT] [WARN] USB gadget unbind before audio reconfigure failed (UDC=%s). Attempting live update.\n",
               GADGET_UDC_NODE);
    } else {
        usleep(20000);
    }

    usb_audio_unlink_config_link();

    if (setup_usb_audio_gadget_layout(playback_channels,
                                      capture_channels,
                                      sample_rate,
                                      sample_size_bytes) < 0) {
        printf("[INIT] [WARN] New USB audio layout failed; rebinding previous gadget layout.\n");
        if (usb_audio_bind_udc_retry() < 0) {
            printf("[INIT] [WARN] Rebind after failed layout update did not recover; attempting explicit layout restore p=%u c=%u rate=%u ssize=%u\n",
                   prev_playback_channels,
                   prev_capture_channels,
                   prev_sample_rate,
                   prev_sample_size);
            (void)setup_usb_audio_gadget_layout(prev_playback_channels,
                                                prev_capture_channels,
                                                prev_sample_rate,
                                                prev_sample_size);
            (void)usb_audio_bind_udc_retry();
        }
        return -1;
    }

    if (usb_audio_bind_udc_retry() < 0) {
        printf("[INIT] [ERR] USB gadget rebind failed after audio layout update; restoring previous layout.\n");
        (void)setup_usb_audio_gadget_layout(prev_playback_channels,
                                            prev_capture_channels,
                                            prev_sample_rate,
                                            prev_sample_size);
        (void)usb_audio_bind_udc_retry();
        return -1;
    }

    printf("[INIT] USB audio gadget reconfigured at runtime.\n");

    return 0;
}

static inline int setup_usb_network_gadget(void) {
    if (ensure_dir(GADGET_ROOT, 0755) < 0) return -1;
    if (ensure_dir(GADGET_NETWORK_FUNC, 0755) < 0) return -1;
    
    if (write_sys_node(GADGET_NETWORK_FUNC "/dev_addr", "02:00:00:00:00:02\n") < 0) return -1;
    if (write_sys_node(GADGET_NETWORK_FUNC "/host_addr", "02:00:00:00:00:01\n") < 0) return -1;

    if (symlink(GADGET_NETWORK_FUNC, GADGET_NETWORK_LINK) < 0 && errno != EEXIST) {
        printf("[INIT] [ERR] Symlink layout assignment failed: %s\n", strerror(errno));
        return -1;
    }



    return 0;
}

static inline int is_network_ready(void) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[INIT] [ERR] Failed to create socket for network interface check: %s\n", strerror(errno));
        return 0;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, USB_NET_IFACE, IFNAMSIZ);
    ifr.ifr_name[IFNAMSIZ - 1] = '\0';

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        close(fd);
        return 0;
    }

    close(fd);
    return 1;
}

static inline int setup_network_interface(void) {
    // must use ioctl because we don't have the "ip" command available in the initramfs

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[INIT] [ERR] Failed to create socket for network interface setup: %s\n", strerror(errno));
        return -1;
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
        return -1;
    }

    inet_pton(AF_INET, USB_NET_NETMASK, &addr->sin_addr);
    if (ioctl(fd, SIOCSIFNETMASK, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to set netmask for usb0: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    if (ioctl(fd, SIOCGIFFLAGS, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to get flags for usb0: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    ifr.ifr_flags |= IFF_UP;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to bring up usb0: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    // Announce the link-local address a few times so the host neighbor table updates quickly.
    for (int i = 0; i < 3; ++i) {
        (void)send_gratuitous_arp(USB_NET_IFACE, USB_NET_SERVER_IP);
        usleep(100 * 1000);
    }

    printf("[INIT] Network up on %s with %s/%s (DHCP disabled).\n",
           USB_NET_IFACE,
           USB_NET_SERVER_IP,
           USB_NET_NETMASK);

    return 0;
}

#endif