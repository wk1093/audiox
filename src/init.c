#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_oss.h"
#include "fb_touch.h"
#include "init_defs.h"
#include "init_helpers.h"
#include "usb_gadget.h"

void debug_input_devices(void) {
    int fd = open("/proc/bus/input/devices", O_RDONLY);
    if (fd < 0) {
        printf("[INIT] Failed to open /proc/bus/input/devices\n");
        return;
    }
    char buf[4096];
    ssize_t bytes = read(fd, buf, sizeof(buf) - 1);
    if (bytes > 0) {
        buf[bytes] = '\0';
        printf("--- CURRENT INPUT DEVICES ---\n%s-----------------------------\n", buf);
    }
    close(fd);
}

static int load_modules_from_list(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("[INIT] [ERR] Could not open module load list %s\n", path);
        return -1;
    }

    char line[768];
    size_t loaded = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }

        if (*p == '\0' || *p == '\n' || *p == '#') {
            continue;
        }

        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[len - 1] = '\0';
            --len;
        }

        if (len == 0) {
            continue;
        }

        printf("  -> Loading: %s\n", p);
        if (load_module(p) < 0) {
            fclose(fp);
            return -1;
        }
        ++loaded;
    }

    fclose(fp);

    if (loaded == 0) {
        printf("[INIT] [ERR] Module list %s was empty.\n", path);
        return -1;
    }

    printf("[INIT] Loaded %zu kernel modules from %s\n", loaded, path);
    return 0;
}

static void show_terminal_cursor(void) {
    printf("\033[?25h");
    fflush(stdout);
}

static void hide_terminal_cursor(void) {
    printf("\033[?25l");
    fflush(stdout);
}

typedef struct ui_layout {
    uint32_t margin;
    uint32_t gap;
    uint32_t btn_w;
    uint32_t btn_h;
} ui_layout_t;

static void get_ui_layout(const fb_ctx_t *fb, ui_layout_t *layout) {
    layout->margin = fb->width / 24;
    if (layout->margin < 8) {
        layout->margin = 8;
    }

    layout->gap = layout->margin;

    uint32_t usable_w = fb->width - (layout->margin * 2) - layout->gap;
    uint32_t usable_h = fb->height - (layout->margin * 2) - layout->gap;

    layout->btn_w = usable_w / 2;
    layout->btn_h = usable_h / 2;

    if (layout->btn_w < 24) {
        layout->btn_w = 24;
    }
    if (layout->btn_h < 24) {
        layout->btn_h = 24;
    }
}

static int touch_map_to_screen(const touch_ctx_t *touch, const fb_ctx_t *fb, int raw_x, int raw_y, int *screen_x, int *screen_y) {
    if (!fb || fb->width == 0 || fb->height == 0 || !screen_x || !screen_y) {
        return -1;
    }

    if (!touch || !touch->has_range) {
        return -1;
    }

    int range_x = touch->max_x - touch->min_x;
    int range_y = touch->max_y - touch->min_y;

    double norm_x = 0.5;
    double norm_y = 0.5;

    if (range_x > 0) {
        norm_x = (double)(raw_x - touch->min_x) / (double)range_x;
    }
    if (range_y > 0) {
        norm_y = (double)(raw_y - touch->min_y) / (double)range_y;
    }

    if (norm_x < 0.0) norm_x = 0.0;
    if (norm_x > 1.0) norm_x = 1.0;
    if (norm_y < 0.0) norm_y = 0.0;
    if (norm_y > 1.0) norm_y = 1.0;

    *screen_x = (int)(norm_x * (double)(fb->width - 1));
    *screen_y = (int)(norm_y * (double)(fb->height - 1));
    return 0;
}

static int ui_hit_button(const fb_ctx_t *fb, int x, int y) {
    if (!fb) {
        return -1;
    }

    ui_layout_t layout;
    get_ui_layout(fb, &layout);

    for (int idx = 0; idx < 4; ++idx) {
        int col = idx % 2;
        int row = idx / 2;

        int bx = (int)layout.margin + col * (int)(layout.btn_w + layout.gap);
        int by = (int)layout.margin + row * (int)(layout.btn_h + layout.gap);
        int bx_end = bx + (int)layout.btn_w;
        int by_end = by + (int)layout.btn_h;

        if (x >= bx && x < bx_end && y >= by && y < by_end) {
            return idx;
        }
    }

    return -1;
}

static void fb_draw_button(const fb_ctx_t *fb, int idx, int enabled) {
    if (!fb || !fb->map || idx < 0 || idx >= 4) {
        return;
    }

    ui_layout_t layout;
    get_ui_layout(fb, &layout);

    uint32_t on_fill[4] = {
        fb_pack_color(fb, 24, 146, 74),
        fb_pack_color(fb, 20, 96, 178),
        fb_pack_color(fb, 176, 96, 24),
        fb_pack_color(fb, 148, 44, 148)
    };
    uint32_t off_fill = fb_pack_color(fb, 42, 46, 52);
    uint32_t on_border = fb_pack_color(fb, 210, 246, 224);
    uint32_t off_border = fb_pack_color(fb, 120, 126, 134);

    int col = idx % 2;
    int row = idx / 2;

    uint32_t bx = layout.margin + (uint32_t)col * (layout.btn_w + layout.gap);
    uint32_t by = layout.margin + (uint32_t)row * (layout.btn_h + layout.gap);
    uint32_t fill = enabled ? on_fill[idx] : off_fill;
    uint32_t border = enabled ? on_border : off_border;

    fb_fill_rect(fb, bx, by, layout.btn_w, layout.btn_h, border);
    fb_fill_rect(fb, bx + 4, by + 4, layout.btn_w - 8, layout.btn_h - 8, fill);
}

static void fb_draw_button_grid(const fb_ctx_t *fb, const int *enabled) {
    if (!fb || !fb->map || !enabled) {
        return;
    }

    ui_layout_t layout;
    get_ui_layout(fb, &layout);

    uint32_t bg = fb_pack_color(fb, 8, 12, 16);
    fb_fill_rect(fb, 0, 0, fb->width, fb->height, bg);

    for (int idx = 0; idx < 4; ++idx) {
        fb_draw_button(fb, idx, enabled[idx]);
    }
}

int main(void) {
    umask(0);

    atexit(show_terminal_cursor);
    hide_terminal_cursor();

    if (mount_basic_filesystems() < 0) {
        goto emergency_halt;
    }

    printf("\n==================================================\n");
    printf("   CUSTOM AUDIO DESK APPLIANCE - INITIALIZER      \n");
    printf("==================================================\n");

    printf("[INIT] Injecting real-time audio gadget driver stack...\n");
    if (load_modules_from_list(MODULE_LOAD_LIST_FILE) < 0) {
        printf("[INIT] [CRIT] Driver framework injection stalled.\n");
        goto emergency_halt;
    }
    sleep(3); // let graphics initialize before we start writing to the framebuffer
    printf("[INIT] Gadget drivers mounted clean.\n");

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

    debug_input_devices();
    
    audio_ctx_t audio;
    fb_ctx_t fb;
    touch_ctx_t touch;

    if (audio_open(&audio) < 0) {
        goto emergency_halt;
    }

    int fb_ready = (fb_open(&fb) == 0);
    int voice_enabled[4] = {
        audio_voice_is_enabled(&audio, 0),
        audio_voice_is_enabled(&audio, 1),
        audio_voice_is_enabled(&audio, 2),
        audio_voice_is_enabled(&audio, 3)
    };

    if (fb_ready) {
        fb_draw_button_grid(&fb, voice_enabled);
    }

    int touch_ready = (touch_open(&touch) == 0);
    if (!touch_ready) {
        printf("[INIT] [WARN] Touch control disabled. Audio will continue playing.\n");
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
            if (fb_ready) {
                int raw_x[TOUCH_MAX_POINTS];
                int raw_y[TOUCH_MAX_POINTS];
                int held[4] = {0, 0, 0, 0};

                int points = touch_poll_points(&touch, raw_x, raw_y, TOUCH_MAX_POINTS);
                if (points > 0) {
                    for (int i = 0; i < points; ++i) {
                        int sx = 0;
                        int sy = 0;
                        if (touch_map_to_screen(&touch, &fb, raw_x[i], raw_y[i], &sx, &sy) == 0) {
                            int button = ui_hit_button(&fb, sx, sy);
                            if (button >= 0 && button < audio_voice_count()) {
                                held[button] = 1;
                            }
                        }
                    }
                }

                for (int i = 0; i < audio_voice_count(); ++i) {
                    if (held[i] == voice_enabled[i]) {
                        continue;
                    }

                    voice_enabled[i] = held[i];
                    audio_set_voice(&audio, i, held[i]);
                    fb_draw_button(&fb, i, held[i]);
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