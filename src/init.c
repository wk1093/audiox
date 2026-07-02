#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <sys/reboot.h>
#include <unistd.h>

static int app_printf_router(const char *fmt, ...);

#define printf(...) app_printf_router(__VA_ARGS__)

#include "audio_oss.h"
#include "fb_touch.h"
#include "config_store.h"
#include "fs_shell.h"
#include "init_defs.h"
#include "init_helpers.h"
#include "midi_runtime.h"
#include "rt_sched.h"
#include "tick_sched.h"
#include "ui_console.h"
#include "usb_gadget.h"

#ifndef DEBUG_SHELL
#define DEBUG_SHELL 0
#endif

#define CONFIG_MOUNT_POINT "/audiox"
#define CONFIG_DEVICE_PATH "/dev/mmcblk0p2"
#define CONFIG_FILE_PATH CONFIG_MOUNT_POINT "/config.txt"

typedef void (*runtime_log_fn_t)(void *ctx, const char *line);

static ui_log_t *g_ui_log = NULL;
static int g_ui_log_active = 0;

static void app_log_bind_ui(ui_log_t *log) {
    g_ui_log = log;
}

static void app_log_set_ui_active(int active) {
    g_ui_log_active = active ? 1 : 0;
}

static int app_printf_router(const char *fmt, ...) {
    char line[384];

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    if (n < 0) {
        return n;
    }

    if (!g_ui_log_active || !g_ui_log) {
        fputs(line, stdout);
        fflush(stdout);
        return n;
    }

    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
        line[len - 1] = '\0';
        --len;
    }

    if (len > 0) {
        ui_log_push(g_ui_log, line);
    }

    return n;
}

static void runtime_log_set_sink(runtime_log_fn_t fn, void *ctx) {
    (void)fn;
    (void)ctx;
}

static void runtime_logf(const char *fmt, ...) {
    char line[192];

    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("%s\n", line);
}

static void runtime_log_line_adapter(void *ctx, const char *line) {
    (void)ctx;
    runtime_logf("%s", line ? line : "");
}


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

static void ui_log_push_adapter(void *ctx, const char *line) {
    ui_log_push((ui_log_t *)ctx, line);
}

static void log_sink_noop(void *ctx, const char *line) {
    (void)ctx;
    (void)line;
}

typedef struct app_state {
    audio_ctx_t audio;
    fb_ctx_t fb;
    touch_ctx_t touch;
    ui_log_t log;
    midi_runtime_t midi;
    config_store_t config;
    int config_enabled;
    int active_tab;
    uint8_t log_filter_mask;
    int fb_ready;
    int touch_ready;
    int voice_enabled[AUDIO_MAX_VOICES];
    uint32_t midi_event_cursor;
    ui_touch_latch_t cfg_touch_latch;
    ui_touch_latch_t tab_touch_latch[UI_TAB_COUNT];
    ui_touch_latch_t log_filter_touch_latch[3];
    ui_touch_latch_t control_touch_latch[3];
    ui_touch_latch_t swipe_touch_latch;
} app_state_t;

typedef struct app_ticks {
    tick_task_t touch_retry;
    tick_task_t touch_range_retry;
    tick_task_t mount_poll;
    tick_task_t periodic_sync;
} app_ticks_t;

typedef enum app_redraw_mode {
    APP_REDRAW_NONE = 0,
    APP_REDRAW_PANEL = 1,
    APP_REDRAW_FULL = 2
} app_redraw_mode_t;

static void app_ticks_init(app_ticks_t *ticks) {
    tick_task_init(&ticks->touch_retry, 200);
    tick_task_init(&ticks->touch_range_retry, 1000);
    tick_task_init(&ticks->mount_poll, 2500);
    tick_task_init(&ticks->periodic_sync, 5000);
}

static void app_state_init(app_state_t *app) {
    memset(app, 0, sizeof(*app));
    ui_log_init(&app->log);
    app_log_bind_ui(&app->log);
    app_log_set_ui_active(0);
    midi_runtime_init(&app->midi, 50);
    config_store_init(&app->config, CONFIG_MOUNT_POINT, CONFIG_DEVICE_PATH, CONFIG_FILE_PATH);
    app->active_tab = UI_TAB_SOUNDBOARD;
    app->log_filter_mask = UI_LOG_FILTER_ALL;
}

static int app_open_audio_thread(app_state_t *app) {
    if (audio_open(&app->audio) < 0) {
        return -1;
    }

    (void)rt_set_current_thread_other("main");

    pthread_t audio_thread;
    int audio_prio = 60;
    if (rt_create_fifo_thread(&audio_thread, audio_thread_main, &app->audio, audio_prio) != 0) {
        printf("[INIT] [WARN] Failed to create FIFO audio thread, retrying with default scheduling.\n");
        if (pthread_create(&audio_thread, NULL, audio_thread_main, &app->audio) != 0) {
            printf("[INIT] [CRIT] Failed to create audio thread.\n");
            audio_close(&app->audio);
            return -1;
        }

        struct sched_param fallback_param;
        memset(&fallback_param, 0, sizeof(fallback_param));
        fallback_param.sched_priority = audio_prio;
        if (pthread_setschedparam(audio_thread, SCHED_FIFO, &fallback_param) != 0) {
            printf("[INIT] [WARN] Audio thread could not switch to SCHED_FIFO: %s\n", strerror(errno));
        }
    }

    return 0;
}

static int app_poll_touch(app_state_t *app, app_ticks_t *ticks, int *raw_x, int *raw_y, int *points) {
    *points = 0;

    if (!app->touch_ready) {
        if (tick_task_due(&ticks->touch_retry)) {
            app->touch_ready = (touch_open(&app->touch) == 0);
            if (app->touch_ready) {
                printf("[INIT] Touch input became available during runtime.\n");
                if (!app->touch.has_range) {
                    ui_log_push(&app->log, "TOUCH RANGE MISSING (FALLBACK)");
                }
            }
        }
        return 0;
    }

    if (!app->touch.has_range && tick_task_due(&ticks->touch_range_retry)) {
        touch_close(&app->touch);
        app->touch_ready = (touch_open(&app->touch) == 0);
        if (app->touch_ready && app->touch.has_range) {
            ui_log_push(&app->log, "TOUCH RANGE RESTORED");
        } else if (app->touch_ready) {
            ui_log_push(&app->log, "TOUCH RANGE STILL MISSING");
        }
    }

    if (!app->touch_ready || !app->fb_ready) {
        return 0;
    }

    *points = touch_poll_points(&app->touch, raw_x, raw_y, TOUCH_MAX_POINTS);
    if (*points < 0) {
        ui_log_push(&app->log, "TOUCH READ FAILED; REOPENING");
        touch_close(&app->touch);
        app->touch_ready = 0;
        return -1;
    }

    return 0;
}

static int app_apply_ui(app_state_t *app,
                        const ui_console_io_t *io) {
    int changed = 0;

    if (io->tab_step != 0) {
        int next = app->active_tab + io->tab_step;
        while (next < 0) {
            next += UI_TAB_COUNT;
        }
        while (next >= UI_TAB_COUNT) {
            next -= UI_TAB_COUNT;
        }
        if (next != app->active_tab) {
            app->active_tab = next;
            changed = 1;
        }
    }

    if (io->selected_tab >= 0 && io->selected_tab < UI_TAB_COUNT && io->selected_tab != app->active_tab) {
        app->active_tab = io->selected_tab;
        changed = 1;
    }

    if (io->config_toggled) {
        app->config_enabled = app->config_enabled ? 0 : 1;
        if (config_store_write_flag(&app->config,
                                    app->config_enabled,
                                    runtime_log_line_adapter,
                                    NULL) == 0) {
            ui_log_push(&app->log, app->config_enabled ? "CONFIG TOGGLE: 1" : "CONFIG TOGGLE: 0");
        } else {
            ui_log_push(&app->log, "CONFIG WRITE FAILED");
        }
        changed = 1;
    }

    for (int i = 0; i < 3; ++i) {
        if (io->log_filter_toggled[i]) {
            uint8_t bit = (uint8_t)(1u << i);
            uint8_t next = app->log_filter_mask ^ bit;
            if (next != 0) {
                app->log_filter_mask = next;
                changed = 1;
            }
        }
    }

    if (io->control_sync) {
        sync();
        ui_log_push(&app->log, "CONTROL: SYNC COMPLETE");
    }

    if (io->control_restart) {
        ui_log_push(&app->log, "CONTROL: RESTART REQUESTED");
        sync();
        if (reboot(RB_AUTOBOOT) < 0) {
            printf("[INIT] [ERR] reboot(RB_AUTOBOOT) failed: %s\n", strerror(errno));
            ui_log_push(&app->log, "CONTROL: RESTART FAILED");
        }
    }

    if (io->control_shutdown) {
        ui_log_push(&app->log, "CONTROL: SHUTDOWN REQUESTED");
        sync();
        if (reboot(RB_POWER_OFF) < 0) {
            printf("[INIT] [ERR] reboot(RB_POWER_OFF) failed: %s\n", strerror(errno));
            ui_log_push(&app->log, "CONTROL: SHUTDOWN FAILED");
        }
    }

    return changed;
}

static int app_refresh_voice_status(app_state_t *app) {
    int changed = 0;
    int count = audio_voice_count(&app->audio);

    for (int i = 0; i < AUDIO_MAX_VOICES; ++i) {
        int next = 0;
        if (i < count) {
            next = audio_voice_is_enabled(&app->audio, i);
        }
        if (next != app->voice_enabled[i]) {
            app->voice_enabled[i] = next;
            changed = 1;
        }
    }

    return changed;
}

static int app_apply_midi_soundboard(app_state_t *app) {
    uint8_t status = 0;
    uint8_t d0 = 0;
    uint8_t d1 = 0;

    if (!midi_consume_event(&app->midi.ctx,
                            &app->midi_event_cursor,
                            &status,
                            &d0,
                            &d1)) {
        return 0;
    }

    uint8_t kind = (uint8_t)(status & 0xF0);
    if (kind != 0x90 || d1 == 0) {
        return 0;
    }

    int slot = (int)d0 - (int)audio_voice_note_base(&app->audio);
    if (slot < 0 || slot >= audio_voice_count(&app->audio)) {
        return 0;
    }

    if (audio_trigger_voice(&app->audio, slot) == 0) {
        char line[96];
        const char *name = audio_voice_name(&app->audio, slot);
        snprintf(line,
                 sizeof(line),
                 "TRIG N%03u SLOT %d %s",
                 (unsigned)d0,
                 slot,
                 name ? name : "");
        ui_log_push(&app->log, line);
        return 1;
    }

    return 0;
}

static void app_boot_draw(app_state_t *app, const char *status_line) {
    if (!app || !app->fb_ready) {
        return;
    }

    const char *title = "AUDIOX";
    const uint32_t title_scale = 3;

    fb_begin_frame(&app->fb);

    uint32_t bg = fb_pack_color(&app->fb, 8, 11, 16);
    uint32_t title_color = fb_pack_color(&app->fb, 230, 238, 248);
    uint32_t status_color = fb_pack_color(&app->fb, 150, 188, 228);
    uint32_t panel = fb_pack_color(&app->fb, 22, 30, 40);

    fb_fill_rect(&app->fb, 0, 0, app->fb.width, app->fb.height, bg);

    int panel_w = (int)app->fb.width - 48;
    if (panel_w < 80) {
        panel_w = (int)app->fb.width;
    }
    int panel_x = ((int)app->fb.width - panel_w) / 2;
    int panel_y = (int)app->fb.height / 3;
    int panel_h = 92;
    if (panel_y + panel_h > (int)app->fb.height - 8) {
        panel_h = (int)app->fb.height - panel_y - 8;
    }
    if (panel_h > 8) {
        fb_fill_rect(&app->fb, (uint32_t)panel_x, (uint32_t)panel_y, (uint32_t)panel_w, (uint32_t)panel_h, panel);
    }

    int tw = ui_text_width(title, title_scale);
    int tx = ((int)app->fb.width - tw) / 2;
    int ty = (int)app->fb.height / 3;
    ui_draw_text(&app->fb, tx, ty, title, title_color, title_scale);

    if (status_line && status_line[0]) {
        int sw = ui_text_width(status_line, 1);
        int sx = ((int)app->fb.width - sw) / 2;
        int sy = ty + ui_text_height(title_scale) + 18;
        ui_draw_text(&app->fb, sx, sy, status_line, status_color, 1);
    }

    (void)fb_present(&app->fb);
}

static void app_boot_status(app_state_t *app, const char *status_line) {
    if (!app || !status_line || !status_line[0]) {
        return;
    }

    ui_log_push(&app->log, status_line);
    app_boot_draw(app, status_line);
}

static void app_try_enable_ui_logging(app_state_t *app) {
    if (!app) {
        return;
    }

    if (!app->fb_ready) {
        app->fb_ready = (fb_open(&app->fb) == 0);
    }

    if (app->fb_ready) {
        app_log_set_ui_active(1);
    }
}

int main(void) {
    umask(0);

    atexit(show_terminal_cursor);
    hide_terminal_cursor();

    if (mount_basic_filesystems() < 0) {
        goto emergency_halt;
    }

    printf("\n========================================\n");
    printf("            audiox init\n");
    printf("========================================\n");

    app_state_t app;
    app_ticks_t ticks;
    app_state_init(&app);
    app_ticks_init(&ticks);
    runtime_log_set_sink(ui_log_push_adapter, &app.log);

    run_temp_fs_shell();

    app_boot_status(&app, "Loading kernel modules...");
    printf("[INIT] Loading kernel modules...\n");
    if (load_modules_from_list(MODULE_LOAD_LIST_FILE) < 0) {
        printf("[INIT] [CRIT] Failed to load kernel modules.\n");
        goto emergency_halt;
    }
    sleep(3);
    printf("[INIT] Kernel modules loaded.\n");

    app_try_enable_ui_logging(&app);
    app_boot_status(&app, app.fb_ready ? "Framebuffer ready. Initializing services..." : "Framebuffer not ready yet. Continuing startup...");

    app_boot_status(&app, "Setting up USB audio gadget...");
    printf("[INIT] Setting up USB audio gadget...\n");
    if (setup_usb_audio_gadget() < 0) {
        printf("[INIT] [CRIT] Failed to configure USB audio gadget.\n");
        goto emergency_halt;
    }

    app_boot_status(&app, "Binding USB gadget to controller...");
    printf("[INIT] Binding gadget to UDC...\n");
    if (write_sys_node(GADGET_UDC_NODE, GADGET_UDC_NAME) < 0) {
        goto emergency_halt;
    }

    sleep(1);

    printf("\n========================================\n");
    printf("             audiox ready\n");
    printf("========================================\n\n");

    app_boot_status(&app, "Enumerating input devices...");
    debug_input_devices();

    app_boot_status(&app, "Loading config store...");
    (void)config_store_ensure(&app.config,
                              &app.config_enabled,
                              runtime_log_line_adapter,
                              NULL);

    ui_log_push(&app.log, "UI READY");
    ui_log_push(&app.log, "WAITING FOR MIDI CONTROLLER...");
    ui_log_push(&app.log, app.config.mount_active ? "CFG FS MOUNTED" : "CFG FS UNMOUNTED");

    app_boot_status(&app, "Starting audio engine...");
    if (app_open_audio_thread(&app) < 0) {
        goto emergency_halt;
    }

    app_try_enable_ui_logging(&app);
    app_boot_status(&app, app.fb_ready ? "Framebuffer active. Starting control surfaces..." : "No framebuffer. Continuing headless runtime...");
    (void)app_refresh_voice_status(&app);

    app_boot_status(&app, "Opening touch input...");
    app.touch_ready = (touch_open(&app.touch) == 0);
    if (!app.touch_ready) {
        printf("[INIT] [WARN] Touch control disabled. Audio will continue playing.\n");
    }

    app_boot_status(&app, "Startup complete. Entering runtime loop...");

    app_redraw_mode_t redraw_mode = APP_REDRAW_FULL;

    while (1) {
        int prev_log_head = app.log.head;
        int prev_log_count = app.log.count;

        int conn_changed = 0;
        int midi_updated = midi_runtime_poll(&app.midi, log_sink_noop, NULL, &conn_changed);
        if (conn_changed) {
            if (redraw_mode < APP_REDRAW_PANEL) {
                redraw_mode = APP_REDRAW_PANEL;
            }
        }
        (void)midi_updated;

        if (app_apply_midi_soundboard(&app)) {
            if (app.active_tab == UI_TAB_SOUNDBOARD && redraw_mode < APP_REDRAW_PANEL) {
                redraw_mode = APP_REDRAW_PANEL;
            }
        }

        if (app_refresh_voice_status(&app)) {
            if (app.active_tab == UI_TAB_SOUNDBOARD && redraw_mode < APP_REDRAW_PANEL) {
                redraw_mode = APP_REDRAW_PANEL;
            }
        }

        if (tick_task_due(&ticks.mount_poll)) {
            int mount_before = app.config.mount_active;
            int mount_after = config_store_refresh_mount_state(&app.config);
            if (mount_before != mount_after) {
                ui_log_push(&app.log, mount_after ? "CFG FS MOUNTED" : "CFG FS UNMOUNTED");
                if (app.active_tab == UI_TAB_CONFIG && redraw_mode < APP_REDRAW_PANEL) {
                    redraw_mode = APP_REDRAW_PANEL;
                }
            }
        }

        int raw_x[TOUCH_MAX_POINTS];
        int raw_y[TOUCH_MAX_POINTS];
        int points = 0;
        if (app_poll_touch(&app, &ticks, raw_x, raw_y, &points) < 0) {
            continue;
        }

        ui_console_model_t model = {
            .voice_enabled = app.voice_enabled,
            .voice_name = app.audio.voice_name,
            .voice_count = audio_voice_count(&app.audio),
            .voice_note_base = audio_voice_note_base(&app.audio),
            .midi_connected = app.midi.ctx.connected,
            .midi_dev = app.midi.ctx.dev_path,
            .config_enabled = app.config_enabled,
            .config_mount_active = app.config.mount_active,
            .active_tab = app.active_tab,
            .log_filter_mask = app.log_filter_mask,
            .log = &app.log
        };
        ui_console_io_t io;

        if (app.fb_ready) {
            ui_console_interact(&app.fb,
                                app.touch_ready ? &app.touch : NULL,
                                app.touch_ready ? raw_x : NULL,
                                app.touch_ready ? raw_y : NULL,
                                points,
                                &app.cfg_touch_latch,
                                app.tab_touch_latch,
                                app.log_filter_touch_latch,
                                app.control_touch_latch,
                                &app.swipe_touch_latch,
                                &model,
                                &io);
            if (app_apply_ui(&app, &io)) {
                if (io.selected_tab >= 0 || io.tab_step != 0) {
                    redraw_mode = APP_REDRAW_FULL;
                } else if (redraw_mode < APP_REDRAW_PANEL) {
                    redraw_mode = APP_REDRAW_PANEL;
                }
            }

            if (app.log.head != prev_log_head || app.log.count != prev_log_count) {
                if (app.active_tab == UI_TAB_LOG && redraw_mode < APP_REDRAW_PANEL) {
                    redraw_mode = APP_REDRAW_PANEL;
                }
            }

            if (redraw_mode != APP_REDRAW_NONE) {
                model.voice_enabled = app.voice_enabled;
                model.voice_name = app.audio.voice_name;
                model.voice_count = audio_voice_count(&app.audio);
                model.voice_note_base = audio_voice_note_base(&app.audio);
                model.midi_connected = app.midi.ctx.connected;
                model.midi_dev = app.midi.ctx.dev_path;
                model.config_enabled = app.config_enabled;
                model.config_mount_active = app.config.mount_active;
                model.active_tab = app.active_tab;
                model.log_filter_mask = app.log_filter_mask;
                model.log = &app.log;

                if (redraw_mode == APP_REDRAW_FULL) {
                    fb_begin_frame(&app.fb);
                    ui_console_frame(&app.fb,
                                     app.touch_ready ? &app.touch : NULL,
                                     app.touch_ready ? raw_x : NULL,
                                     app.touch_ready ? raw_y : NULL,
                                     points,
                                     &app.cfg_touch_latch,
                                     app.tab_touch_latch,
                                     app.log_filter_touch_latch,
                                     app.control_touch_latch,
                                     &app.swipe_touch_latch,
                                     &model,
                                     &io);
                    (void)fb_present(&app.fb);
                } else {
                    if (app.fb.pageflip_enabled) {
                        app.fb.draw_page = app.fb.front_page;
                        app.fb.draw_offset = app.fb.front_page ? app.fb.page_len : 0;
                    }
                    ui_console_draw_tab_bar_only(&app.fb, &model);
                    ui_console_draw_active_panel_only(&app.fb, &model);
                }
                redraw_mode = APP_REDRAW_NONE;
            }
        } else {
            (void)ui_im_rising_edge(0, &app.cfg_touch_latch);
            for (int i = 0; i < UI_TAB_COUNT; ++i) {
                (void)ui_im_rising_edge(0, &app.tab_touch_latch[i]);
            }
            for (int i = 0; i < 3; ++i) {
                (void)ui_im_rising_edge(0, &app.log_filter_touch_latch[i]);
            }
            for (int i = 0; i < 3; ++i) {
                (void)ui_im_rising_edge(0, &app.control_touch_latch[i]);
            }
            (void)ui_im_rising_edge(0, &app.swipe_touch_latch);
        }

        if (tick_task_due(&ticks.periodic_sync)) {
            sync();
        }

        usleep(2000);
    }

    midi_runtime_disconnect(&app.midi, ui_log_push_adapter, &app.log);
    audio_close(&app.audio);
    if (app.touch_ready) {
        touch_close(&app.touch);
    }
    if (app.fb_ready) {
        fb_close(&app.fb);
    }
    return 0;

emergency_halt:
    printf("[INIT] Critical startup error. Halting.\n");
    while (1) {
        sleep(60);
    }
    return 1;
}