#ifndef USB_GADGET_H
#define USB_GADGET_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>

#include "init_defs.h"
#include "init_helpers.h"

static inline int setup_usb_audio_gadget(void) {
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

    if (write_sys_node(GADGET_UAC2_FUNC "/c_chmask", "3\n") < 0) return -1;
    if (write_sys_node(GADGET_UAC2_FUNC "/c_srate", "44100\n") < 0) return -1;
    if (write_sys_node(GADGET_UAC2_FUNC "/c_ssize", "2\n") < 0) return -1;

    if (write_sys_node(GADGET_UAC2_FUNC "/p_chmask", "3\n") < 0) return -1;
    if (write_sys_node(GADGET_UAC2_FUNC "/p_srate", "44100\n") < 0) return -1;
    if (write_sys_node(GADGET_UAC2_FUNC "/p_ssize", "2\n") < 0) return -1;

    if (symlink(GADGET_UAC2_FUNC, GADGET_CONFIG_LINK) < 0 && errno != EEXIST) {
        printf("[INIT] [ERR] Symlink layout assignment failed: %s\n", strerror(errno));
        return -1;
    }

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
    // We will poll before calling the setup_network_interface() function to ensure that the network interface is ready to be configured.
    // This is necessary because the network interface may not be immediately available after the USB gadget is set up.
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[INIT] [ERR] Failed to create socket for network interface check: %s\n", strerror(errno));
        return 0;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "usb0", IFNAMSIZ);
    close(fd);
    return 1;
}

static inline int setup_network_interface(void) {
    // must use ioctl because we don't have the "ip" command available in the initramfs

    // 169.254.1.2

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        printf("[INIT] [ERR] Failed to create socket for network interface setup: %s\n", strerror(errno));
        return -1;
    }

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, "usb0", IFNAMSIZ);

    struct sockaddr_in *addr = (struct sockaddr_in *)&ifr.ifr_addr;
    addr->sin_family = AF_INET;
    inet_pton(AF_INET, "169.254.1.2", &addr->sin_addr);
    if (ioctl(fd, SIOCSIFADDR, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to set IP address for usb0: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    inet_pton(AF_INET, "255.255.0.0", &addr->sin_addr);
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
    ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
    if (ioctl(fd, SIOCSIFFLAGS, &ifr) < 0) {
        printf("[INIT] [ERR] Failed to bring up usb0: %s\n", strerror(errno));
        close(fd);
        return -1;
    }
    close(fd);

    return 0;
}

#endif