#ifndef INIT_DEFS_H
#define INIT_DEFS_H

#include <stddef.h>

#ifndef KERNEL_VERSION
#define KERNEL_VERSION "6.18.36-v8+"
#endif

#define SAMPLE_RATE 44100
#define FREQUENCY 440.0
#define AMPLITUDE 16000
#define PI 3.14159265358979323846
#define BUFFER_FRAMES 512

#define MODULE_LOAD_LIST_FILE "/etc/module-load.list"

#define GADGET_ROOT "/sys/kernel/config/usb_gadget/g1"
#define GADGET_UAC2_FUNC GADGET_ROOT "/functions/uac2.usb0"
#define GADGET_CONFIG_LINK GADGET_ROOT "/configs/c.1/uac2.usb0"
#define GADGET_UDC_NODE GADGET_ROOT "/UDC"
#define GADGET_UDC_NAME "fe980000.usb\n"

#endif
