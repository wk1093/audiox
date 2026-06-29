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

static const char *const MODULE_STACK[] = {
    "/lib/modules/" KERNEL_VERSION "/kernel/sound/core/snd.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/sound/core/snd-timer.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/sound/core/snd-pcm.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/sound/core/oss/snd-pcm-oss.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/usb/gadget/libcomposite.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/usb/gadget/function/u_audio.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/usb/gadget/function/usb_f_uac2.ko"
};

#define MODULE_COUNT (sizeof(MODULE_STACK) / sizeof(MODULE_STACK[0]))

static const char *const TOUCH_MODULE_STACK[] = {
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/video/backlight/backlight.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/gpu/drm/drm_panel_orientation_quirks.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/gpu/drm/drm.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/base/regmap/regmap-i2c.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/regulator/rpi-panel-attiny-regulator.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/gpu/drm/panel/panel-raspberrypi-touchscreen.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/input/touchscreen/raspberrypi-ts.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/hid/hid-multitouch.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/input/touchscreen/usbtouchscreen.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/input/touchscreen/goodix_ts.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/input/touchscreen/edt-ft5x06.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/hid/i2c-hid/i2c-hid.ko",
    "/lib/modules/" KERNEL_VERSION "/kernel/drivers/hid/i2c-hid/i2c-hid-of.ko"
};

#define TOUCH_MODULE_COUNT (sizeof(TOUCH_MODULE_STACK) / sizeof(TOUCH_MODULE_STACK[0]))

#define GADGET_ROOT "/sys/kernel/config/usb_gadget/g1"
#define GADGET_UAC2_FUNC GADGET_ROOT "/functions/uac2.usb0"
#define GADGET_CONFIG_LINK GADGET_ROOT "/configs/c.1/uac2.usb0"
#define GADGET_UDC_NODE GADGET_ROOT "/UDC"
#define GADGET_UDC_NAME "fe980000.usb\n"

#endif
