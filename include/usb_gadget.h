#ifndef USB_GADGET_H
#define USB_GADGET_H

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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

#endif
