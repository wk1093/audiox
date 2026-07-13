#pragma once

#include <cstddef>

#ifndef KERNEL_VERSION
#define KERNEL_VERSION "6.18.36-v8+"
#endif

#define SAMPLE_RATE 44100
#define FREQUENCY 440.0
#define AMPLITUDE 16000
#define PI 3.14159265358979323846
#define BUFFER_FRAMES 128

#define MODULE_LOAD_LIST_FILE "/etc/module-load.list"
#define MODULE_LOAD_BASE_LIST_FILE "/etc/module-load.base.list"
#define MODULE_LOAD_NORMAL_LIST_FILE "/etc/module-load.normal.list"
#define BOOT_MODULE_LOAD_LIST_FILE "/etc/bootmodule-load.list"
#define BOOT_LOGO_PPM_PATH "/etc/logo_boot.ppm"
#define PROGRAM_RUNTIME_ROOT "/program"
#define PROGRAM_INITRAMFS_NAME "program.cpio.gz"

#define GADGET_ROOT "/sys/kernel/config/usb_gadget/g1"
#define GADGET_UAC2_FUNC GADGET_ROOT "/functions/uac2.usb0"
#define GADGET_CONFIG_LINK GADGET_ROOT "/configs/c.1/uac2.usb0"
#define GADGET_NETWORK_FUNC GADGET_ROOT "/functions/ncm.usb0"
#define GADGET_NETWORK_LINK GADGET_ROOT "/configs/c.1/ncm.usb0"
#define GADGET_UDC_NODE GADGET_ROOT "/UDC"
#define GADGET_UDC_NAME "fe980000.usb\n"
#define BOOT_DEVICE_PATH "/dev/mmcblk0p1"
#define CONFIG_DEVICE_PATH "/dev/mmcblk0p2"
#define BOOT_MOUNT_POINT "/mnt/boot"
#define ROOTFS_STAGING_MOUNT_POINT "/mnt/rootfs"

#define RET_OK 0 // do nothing extra
#define RET_ERR -1 // fatal error, halt
#define RET_WARN 1 // non-fatal warning, continue but log it

#define WARN_UNUSED __attribute__((warn_unused_result))

#define ROOT_MOUNT_POINT "/audiox"
#define STAGING_DIR_NAME "staging"
#define PROGRAM_STAGED_PATH ROOT_MOUNT_POINT "/" STAGING_DIR_NAME "/" PROGRAM_INITRAMFS_NAME

#define USB_NET_IFACE "usb0"
#define USB_NET_SERVER_IP "169.254.1.2"
#define USB_NET_NETMASK "255.255.0.0"

#define USB_GADGET_IN_GAIN 10.0f