#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "audio_oss.h"
#include "fb_touch.h"
#include "init_defs.h"
#include "init_helpers.h"
#include "midi_input.h"
#include "ui_fb.h"
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

static int set_current_thread_other(const char *tag) {
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    if (sched_setscheduler(0, SCHED_OTHER, &param) < 0) {
        printf("[INIT] [WARN] Failed to set %s thread to SCHED_OTHER: %s\n", tag, strerror(errno));
        return -1;
    }
    return 0;
}

static int create_fifo_audio_thread(pthread_t *thread, void *(*entry)(void *), void *arg, int priority) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        printf("[INIT] [WARN] pthread_attr_init failed for audio thread.\n");
        return pthread_create(thread, NULL, entry, arg);
    }

    int attr_ok = 1;
    if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0) {
        attr_ok = 0;
    }
    if (attr_ok && pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
        attr_ok = 0;
    }

    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;
    if (attr_ok && pthread_attr_setschedparam(&attr, &param) != 0) {
        attr_ok = 0;
    }

    int rc = pthread_create(thread, attr_ok ? &attr : NULL, entry, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

static void *audio_thread_main(void *arg) {
    audio_ctx_t *audio = (audio_ctx_t *)arg;
    while (1) {
        audio_write_next(audio);
    }
    return NULL;
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
    uint32_t title_h;
    uint32_t btn_w;
    uint32_t btn_h;
    uint32_t log_x;
    uint32_t log_y;
    uint32_t log_w;
    uint32_t log_h;
} ui_layout_t;

typedef struct ui_log {
    char lines[64][96];
    int count;
    int head;
} ui_log_t;

static void ui_log_init(ui_log_t *log) {
    memset(log, 0, sizeof(*log));
}

static void ui_log_push(ui_log_t *log, const char *line) {
    if (!log || !line) {
        return;
    }

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(log->lines[log->head], sizeof(log->lines[log->head]), "%s", line);
#pragma GCC diagnostic pop
    log->head = (log->head + 1) % 64;
    if (log->count < 64) {
        ++log->count;
    }
}

static int ui_log_get_line(const ui_log_t *log, int idx, const char **out) {
    if (!log || !out || idx < 0 || idx >= log->count) {
        return -1;
    }

    int start = (log->head - log->count + 64) % 64;
    int slot = (start + idx) % 64;
    *out = log->lines[slot];
    return 0;
}

static void ui_log_push_adapter(void *ctx, const char *line) {
    ui_log_push((ui_log_t *)ctx, line);
}

static void get_ui_layout(const fb_ctx_t *fb, ui_layout_t *layout) {
    layout->margin = fb->width / 24;
    if (layout->margin < 8) {
        layout->margin = 8;
    }

    layout->gap = layout->margin / 2;
    if (layout->gap < 6) {
        layout->gap = 6;
    }

    layout->title_h = (uint32_t)ui_text_height(2) + layout->gap;

    uint32_t usable_w = fb->width - (layout->margin * 2) - layout->gap;
    layout->btn_w = usable_w / 2;
    layout->btn_h = fb->height / 5;

    if (layout->btn_w < 24) {
        layout->btn_w = 24;
    }
    if (layout->btn_h < 48) {
        layout->btn_h = 48;
    }

    uint32_t btn_y = layout->margin + layout->title_h;
    layout->log_x = layout->margin;
    layout->log_y = btn_y + layout->btn_h + layout->gap;
    layout->log_w = fb->width - (layout->margin * 2);
    layout->log_h = fb->height - layout->log_y - layout->margin;
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

    for (int idx = 0; idx < 2; ++idx) {
        int col = idx % 2;

        int bx = (int)layout.margin + col * (int)(layout.btn_w + layout.gap);
        int by = (int)layout.margin + (int)layout.title_h;
        int bx_end = bx + (int)layout.btn_w;
        int by_end = by + (int)layout.btn_h;

        if (x >= bx && x < bx_end && y >= by && y < by_end) {
            return idx;
        }
    }

    return -1;
}

static void fb_draw_button(const fb_ctx_t *fb, int idx, int enabled) {
    if (!fb || !fb->map || idx < 0 || idx >= 2) {
        return;
    }

    ui_layout_t layout;
    get_ui_layout(fb, &layout);

    static const char *labels[2] = {
        "VOICE 1",
        "VOICE 2"
    };

    uint32_t on_fill[2] = {
        fb_pack_color(fb, 24, 146, 74),
        fb_pack_color(fb, 20, 96, 178)
    };
    uint32_t off_fill = fb_pack_color(fb, 42, 46, 52);
    uint32_t on_border = fb_pack_color(fb, 210, 246, 224);
    uint32_t off_border = fb_pack_color(fb, 120, 126, 134);

    int col = idx % 2;

    uint32_t bx = layout.margin + (uint32_t)col * (layout.btn_w + layout.gap);
    uint32_t by = layout.margin + layout.title_h;
    ui_button_theme_t theme;
    memset(&theme, 0, sizeof(theme));
    theme.fill = off_fill;
    theme.fill_active = on_fill[idx];
    theme.border = off_border;
    theme.border_active = on_border;
    theme.text = fb_pack_color(fb, 210, 216, 226);
    theme.text_active = fb_pack_color(fb, 248, 252, 255);

    ui_draw_button(fb, bx, by, layout.btn_w, layout.btn_h, labels[idx], &theme, enabled, 2);
}

static void fb_draw_log_panel(const fb_ctx_t *fb, const ui_layout_t *layout, const ui_log_t *log, int midi_connected, const char *midi_dev) {
    if (!fb || !fb->map || !layout || !log) {
        return;
    }

    uint32_t panel_border = fb_pack_color(fb, 112, 122, 136);
    uint32_t panel_fill = fb_pack_color(fb, 16, 20, 24);
    uint32_t header_color = fb_pack_color(fb, 224, 232, 242);
    uint32_t text_color = fb_pack_color(fb, 182, 196, 212);
    uint32_t status_ok = fb_pack_color(fb, 128, 224, 148);
    uint32_t status_wait = fb_pack_color(fb, 236, 184, 96);

    ui_fill_rect(fb, layout->log_x, layout->log_y, layout->log_w, layout->log_h, panel_border);
    if (layout->log_w > 4 && layout->log_h > 4) {
        ui_fill_rect(fb, layout->log_x + 2, layout->log_y + 2, layout->log_w - 4, layout->log_h - 4, panel_fill);
    }

    int text_x = (int)layout->log_x + 8;
    int text_y = (int)layout->log_y + 8;
    ui_draw_text(fb, text_x, text_y, "MIDI LOG", header_color, 1);

    char status_line[160];
    snprintf(status_line,
             sizeof(status_line),
             "STATUS: %s%s%s",
             midi_connected ? "CONNECTED" : "WAITING",
             (midi_connected && midi_dev && midi_dev[0] != '\0') ? " " : "",
             (midi_connected && midi_dev && midi_dev[0] != '\0') ? midi_dev : "");
    ui_draw_text(fb, text_x + 72, text_y, status_line, midi_connected ? status_ok : status_wait, 1);

    int line_h = ui_text_height(1) + 2;
    int first_line_y = text_y + ui_text_height(1) + 8;
    int max_lines = ((int)layout->log_h - (first_line_y - (int)layout->log_y) - 8) / line_h;
    if (max_lines < 1) {
        return;
    }

    int start_idx = log->count - max_lines;
    if (start_idx < 0) {
        start_idx = 0;
    }

    int row = 0;
    for (int i = start_idx; i < log->count; ++i) {
        const char *line = NULL;
        if (ui_log_get_line(log, i, &line) == 0 && line) {
            ui_draw_text(fb, text_x, first_line_y + (row * line_h), line, text_color, 1);
        }
        ++row;
    }
}

static void fb_draw_scene(const fb_ctx_t *fb, const int *enabled, const ui_log_t *log, int midi_connected, const char *midi_dev) {
    if (!fb || !fb->map || !enabled || !log) {
        return;
    }

    ui_layout_t layout;
    get_ui_layout(fb, &layout);

    uint32_t bg = fb_pack_color(fb, 8, 12, 16);
    ui_fill_rect(fb, 0, 0, fb->width, fb->height, bg);

    uint32_t title_color = fb_pack_color(fb, 222, 230, 240);
    int title_x = (int)(layout.margin);
    int title_y = (int)(layout.margin / 2);
    ui_draw_text(fb, title_x, title_y, "TOUCH VOICES + MIDI LOG", title_color, 2);

    for (int idx = 0; idx < 2; ++idx) {
        fb_draw_button(fb, idx, enabled[idx]);
    }

    fb_draw_log_panel(fb, &layout, log, midi_connected, midi_dev);
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
    ui_log_t log;
    midi_ctx_t midi;

    ui_log_init(&log);
    midi_ctx_init(&midi);
    ui_log_push(&log, "UI READY");
    ui_log_push(&log, "WAITING FOR MIDI CONTROLLER...");

    if (audio_open(&audio) < 0) {
        goto emergency_halt;
    }

    (void)set_current_thread_other("main");

    pthread_t audio_thread;
    int audio_prio = 60;
    if (create_fifo_audio_thread(&audio_thread, audio_thread_main, &audio, audio_prio) != 0) {
        printf("[INIT] [WARN] Failed to create FIFO audio thread, retrying with default scheduling.\n");
        if (pthread_create(&audio_thread, NULL, audio_thread_main, &audio) != 0) {
            printf("[INIT] [CRIT] Failed to create audio thread.\n");
            audio_close(&audio);
            goto emergency_halt;
        }

        struct sched_param fallback_param;
        memset(&fallback_param, 0, sizeof(fallback_param));
        fallback_param.sched_priority = audio_prio;
        if (pthread_setschedparam(audio_thread, SCHED_FIFO, &fallback_param) != 0) {
            printf("[INIT] [WARN] Audio thread could not switch to SCHED_FIFO: %s\n", strerror(errno));
        }
    }

    int fb_ready = (fb_open(&fb) == 0);
    int voice_enabled[4] = {
        audio_voice_is_enabled(&audio, 0),
        audio_voice_is_enabled(&audio, 1),
        audio_voice_is_enabled(&audio, 2),
        audio_voice_is_enabled(&audio, 3)
    };

    if (fb_ready) {
        fb_draw_scene(&fb, voice_enabled, &log, midi.connected, midi.dev_path);
    }

    int touch_ready = (touch_open(&touch) == 0);
    if (!touch_ready) {
        printf("[INIT] [WARN] Touch control disabled. Audio will continue playing.\n");
    }

    int touch_retry_ticks = 0;
    int hold_grace_ticks[2] = {0, 0};
    const int hold_release_grace = 12;
    int midi_probe_ticks = 0;

    while (1) {
        int ui_dirty = 0;

        if (++midi_probe_ticks >= 50 || midi.fd >= 0) {
            int conn_changed = 0;
            ui_dirty |= midi_poll(&midi, ui_log_push_adapter, &log, &conn_changed);
            if (conn_changed) {
                midi_probe_ticks = 0;
            }
        }

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
                int touched_now[2] = {0, 0};

                int points = touch_poll_points(&touch, raw_x, raw_y, TOUCH_MAX_POINTS);
                if (points > 0) {
                    for (int i = 0; i < points; ++i) {
                        int sx = 0;
                        int sy = 0;
                        if (touch_map_to_screen(&touch, &fb, raw_x[i], raw_y[i], &sx, &sy) == 0) {
                            int button = ui_hit_button(&fb, sx, sy);
                            if (button >= 0 && button < 2) {
                                touched_now[button] = 1;
                            }
                        }
                    }
                }

                for (int i = 0; i < 2; ++i) {
                    if (touched_now[i]) {
                        hold_grace_ticks[i] = hold_release_grace;
                    } else if (hold_grace_ticks[i] > 0) {
                        --hold_grace_ticks[i];
                    }

                    int should_enable = (hold_grace_ticks[i] > 0) ? 1 : 0;

                    if (should_enable == voice_enabled[i]) {
                        continue;
                    }

                    voice_enabled[i] = should_enable;
                    audio_set_voice(&audio, i, should_enable);
                    ui_dirty = 1;
                }
            }
        }

        if (fb_ready && ui_dirty) {
            fb_draw_scene(&fb, voice_enabled, &log, midi.connected, midi.dev_path);
        }

        usleep(2000);
    }

    midi_disconnect(&midi, ui_log_push_adapter, &log);
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