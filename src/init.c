#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_oss.h"
#include "fb_touch.h"
#include "init_defs.h"
#include "init_helpers.h"
#include "usb_gadget.h"

int main(void) {
    umask(0);

    if (mount_basic_filesystems() < 0) {
        goto emergency_halt;
    }

    printf("\n==================================================\n");
    printf("   CUSTOM AUDIO DESK APPLIANCE - INITIALIZER      \n");
    printf("==================================================\n");

    printf("[INIT] Injecting real-time audio gadget driver stack...\n");
    for (size_t i = 0; i < MODULE_COUNT; ++i) {
        printf("  -> Loading: %s\n", MODULE_STACK[i]);
        if (load_module(MODULE_STACK[i]) < 0) {
            printf("[INIT] [CRIT] Driver framework injection stalled.\n");
            goto emergency_halt;
        }
    }
    printf("[INIT] Gadget drivers mounted clean.\n");

    printf("[INIT] Loading optional touch/input modules...\n");
    for (size_t i = 0; i < TOUCH_MODULE_COUNT; ++i) {
        printf("  -> Optional: %s\n", TOUCH_MODULE_STACK[i]);
        if (load_module(TOUCH_MODULE_STACK[i]) < 0) {
            printf("[INIT] [WARN] Optional touch module unavailable: %s\n", TOUCH_MODULE_STACK[i]);
        }
    }

    printf("[INIT] Synthesizing UAC2 blueprint structures...\n");
    if (setup_usb_audio_gadget() < 0) {
        printf("[INIT] [CRIT] Structural assembly error tracked during ConfigFS generation.\n");
        goto emergency_halt;
    }

    printf("[INIT] Binding operational stack to DesignWare OTG Core controller...\n");
    if (write_sys_node(GADGET_UDC_NODE, GADGET_UDC_NAME) < 0) {
        goto emergency_halt;
    }

    sleep(1);

    printf("\n==================================================\n");
    printf("  SUCCESS: DEVICE OPERATIONAL ENGINES ARE LIVE     \n");
    printf("==================================================\n\n");

    audio_ctx_t audio;
    fb_ctx_t fb;
    touch_ctx_t touch;

    if (audio_open(&audio) < 0) {
        goto emergency_halt;
    }

    int fb_ready = (fb_open(&fb) == 0);
    if (fb_ready) {
        fb_draw_status(&fb, audio.enabled);
    }

    int touch_ready = (touch_open(&touch) == 0);
    if (!touch_ready) {
        printf("[INIT] [WARN] Touch toggle disabled. Audio will continue playing.\n");
    }

    int touch_retry_ticks = 0;

    while (1) {
        if (!touch_ready) {
            if (++touch_retry_ticks >= 200) {
                touch_retry_ticks = 0;
                touch_ready = (touch_open(&touch) == 0);
                if (touch_ready) {
                    printf("[INIT] Touch input became available during runtime.\n");
                }
            }
        } else {
            int x = 0;
            int y = 0;
            if (touch_poll_toggle(&touch, &x, &y)) {
                audio.enabled = !audio.enabled;
                printf("[INIT] Touch toggle at x=%d y=%d -> sine %s\n", x, y, audio.enabled ? "ON" : "OFF");
                if (fb_ready) {
                    fb_draw_status(&fb, audio.enabled);
                }
            }
        }

        audio_write_next(&audio);
    }

    audio_close(&audio);
    if (touch_ready) {
        touch_close(&touch);
    }
    if (fb_ready) {
        fb_close(&fb);
    }
    return 0;

emergency_halt:
    printf("[INIT] System tracking context faulted. Entering panic protection loop.\n");
    while (1) {
        sleep(60);
    }
    return 1;
}