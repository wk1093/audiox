#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
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
#include "app_config.h"
#include "config_store.h"
#include "fs_shell.h"
#include "http_server.h"
#include "init_defs.h"
#include "init_helpers.h"
#include "midi_runtime.h"
#include "rt_sched.h"
#include "tick_sched.h"
#include "ui_console.h"
#include "ui_ppm.h"
#include "usb_gadget.h"

#ifndef DEBUG_SHELL
#define DEBUG_SHELL 0
#endif

#define CONFIG_MOUNT_POINT "/audiox"
#define CONFIG_REAL_FILE_PATH CONFIG_MOUNT_POINT "/config.txt"
#define CONFIG_STAGING_FILE_PATH CONFIG_MOUNT_POINT "/config.staging.txt"

typedef void (*runtime_log_fn_t)(void *ctx, const char *line);
typedef struct app_state app_state_t;

static void app_boot_status(app_state_t *app, const char *status_line);

static ui_log_t *g_ui_log = NULL;
static int g_ui_log_active = 0;
static app_state_t *g_boot_status_app = NULL;
static int g_boot_status_active = 0;
static char g_boot_last_error[384];

static int app_line_is_boot_error(const char *line) {
    if (!line || !line[0]) {
        return 0;
    }

    if (strstr(line, "[ERR]") || strstr(line, "[CRIT]") || strstr(line, "FAILED") || strstr(line, "Failed")) {
        return 1;
    }
    return 0;
}

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

        if (g_boot_status_active && g_boot_status_app && app_line_is_boot_error(line)) {
            size_t copy_len = strnlen(line, sizeof(g_boot_last_error) - 1);
            memcpy(g_boot_last_error, line, copy_len);
            g_boot_last_error[copy_len] = '\0';
            app_boot_status(g_boot_status_app, g_boot_last_error);
        }
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

struct app_state {
    audio_ctx_t audio;
    fb_ctx_t fb;
    touch_ctx_t touch;
    ui_log_t log;
    midi_runtime_t midi;
    config_store_t config;
    int active_tab;
    uint8_t log_filter_mask;
    int fb_ready;
    int touch_ready;
    int voice_enabled[AUDIO_MAX_VOICES];
    uint32_t midi_event_cursor;
    ui_touch_latch_t cfg_touch_latch;
    ui_touch_latch_t cfg_reload_touch_latch;
    ui_touch_latch_t tab_touch_latch[UI_TAB_COUNT];
    ui_touch_latch_t log_filter_touch_latch[3];
    ui_touch_latch_t control_touch_latch[3];
    ui_touch_latch_t swipe_touch_latch;
    ui_ppm_image_t boot_logo;
    http_server_t http;
    int http_ready;
    app_runtime_config_t runtime_cfg;
};

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
    config_store_init(&app->config,
                      CONFIG_MOUNT_POINT,
                      CONFIG_DEVICE_PATH,
                      CONFIG_REAL_FILE_PATH,
                      CONFIG_STAGING_FILE_PATH);
    app_runtime_config_defaults(&app->runtime_cfg);
    app->active_tab = UI_TAB_SOUNDBOARD;
    app->log_filter_mask = UI_LOG_FILTER_ALL;
}

static int app_apply_runtime_config(app_state_t *app,
                                    const app_runtime_config_t *cfg,
                                    int runtime_reload) {
    if (!app || !cfg) {
        return -1;
    }

    app->runtime_cfg = *cfg;

    if (!app_runtime_config_is_valid(cfg)) {
        printf("[INIT] [ERR] Invalid USB layout in config file.\n");
        return -1;
    }

    if (runtime_reload) {
        return usb_audio_gadget_reconfigure_layout(cfg->usb_playback_channels,
                                                   cfg->usb_capture_channels,
                                                   cfg->usb_sample_rate,
                                                   cfg->usb_sample_size);
    }

    return setup_usb_audio_gadget_layout(cfg->usb_playback_channels,
                                         cfg->usb_capture_channels,
                                         cfg->usb_sample_rate,
                                         cfg->usb_sample_size);
}

static int app_reload_runtime_config(app_state_t *app, int runtime_reload) {
    if (!app) {
        return -1;
    }

    if (runtime_reload) {
        audio_runtime_set_suspended(&app->audio, 1);
        usleep(50000);
    }

    const char *primary_path = runtime_reload ? CONFIG_STAGING_FILE_PATH : CONFIG_REAL_FILE_PATH;

    app_runtime_config_t cfg;
    app_runtime_config_defaults(&cfg);
    if (app_load_runtime_config_file(primary_path, &cfg) < 0) {
        printf("[INIT] [WARN] Failed to read runtime config file %s\n", primary_path);
        if (runtime_reload) {
            audio_runtime_set_suspended(&app->audio, 0);
        }
        return -1;
    }

    if (app_apply_runtime_config(app, &cfg, runtime_reload) < 0) {
        printf("[INIT] [ERR] Failed to apply runtime config from %s.\n", primary_path);

        if (runtime_reload) {
            app_runtime_config_t rollback_cfg;
            app_runtime_config_defaults(&rollback_cfg);

            if (app_load_runtime_config_file(CONFIG_REAL_FILE_PATH, &rollback_cfg) == 0 &&
                app_apply_runtime_config(app, &rollback_cfg, 1) == 0) {
                printf("[INIT] Rolled back to previous runtime config from %s\n", CONFIG_REAL_FILE_PATH);
            } else {
                printf("[INIT] [CRIT] Failed to roll back runtime config from %s\n", CONFIG_REAL_FILE_PATH);
            }

            (void)usb_audio_bind_udc_retry();
        }

        if (runtime_reload) {
            audio_runtime_set_suspended(&app->audio, 0);
        }

        return -1;
    }

    if (runtime_reload) {
        if (config_store_copy_file(CONFIG_STAGING_FILE_PATH, CONFIG_REAL_FILE_PATH) < 0) {
            printf("[INIT] [ERR] Applied staging config but failed to promote to %s\n", CONFIG_REAL_FILE_PATH);
            audio_runtime_set_suspended(&app->audio, 0);
            return -1;
        }
        printf("[INIT] Promoted staging config %s -> %s\n", CONFIG_STAGING_FILE_PATH, CONFIG_REAL_FILE_PATH);
    }

    printf("[INIT] Runtime config loaded from %s: playback=%u capture=%u rate=%u ssize=%u\n",
           primary_path,
           cfg.usb_playback_channels,
           cfg.usb_capture_channels,
           cfg.usb_sample_rate,
           cfg.usb_sample_size);

    if (runtime_reload) {
        audio_runtime_set_suspended(&app->audio, 0);
    }

    return 0;
}

static int app_load_boot_logo(app_state_t *app) {
    if (!app) {
        return -1;
    }
    return ui_ppm_load(&app->boot_logo, BOOT_LOGO_PPM_PATH);
}

static void app_draw_boot_logo(const app_state_t *app,
                               int x,
                               int y,
                               int max_w,
                               int max_h) {
    if (!app) {
        return;
    }
    ui_ppm_draw(&app->boot_logo, &app->fb, x, y, max_w, max_h);
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

    if (io->config_reloaded) {
        if (app_reload_runtime_config(app, 1) == 0) {
            ui_log_push(&app->log, "CONFIG RELOAD: OK");
        } else {
            ui_log_push(&app->log, "CONFIG RELOAD: FAILED");
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
    audio_control_snapshot_t snapshot;

    memset(&snapshot, 0, sizeof(snapshot));
    audio_copy_control_snapshot(&app->audio, &snapshot);

    for (int i = 0; i < AUDIO_MAX_VOICES; ++i) {
        int next = 0;
        if (i < snapshot.voice_count) {
            next = snapshot.voice_enabled[i] ? 1 : 0;
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

static int app_http_trigger_soundboard_slot(void *ctx, int slot) {
    app_state_t *app = (app_state_t *)ctx;
    if (!app) {
        return -1;
    }

    int max_slots = audio_voice_count(&app->audio);
    if (slot < 0 || slot >= max_slots) {
        return -1;
    }

    if (audio_trigger_voice(&app->audio, slot) != 0) {
        return -1;
    }

    printf("[INIT] HTTP trigger slot %d\n", slot);
    return 0;
}

static int app_http_reload_config(void *ctx) {
    app_state_t *app = (app_state_t *)ctx;
    if (!app) {
        return -1;
    }

    if (app_reload_runtime_config(app, 1) < 0) {
        return -1;
    }

    ui_log_push(&app->log, "CONFIG RELOADED");
    printf("[INIT] HTTP config reload completed.\n");
    return 0;
}

static void app_boot_draw(app_state_t *app, const char *status_line) {
    if (!app || !app->fb_ready) {
        return;
    }

    (void)app_load_boot_logo(app);

    fb_begin_frame(&app->fb);

    uint32_t bg = fb_pack_color(&app->fb, 8, 11, 16);
    uint32_t status_color = fb_pack_color(&app->fb, 150, 188, 228);

    fb_fill_rect(&app->fb, 0, 0, app->fb.width, app->fb.height, bg);

    int logo_w = (int)app->fb.width - 48;
    if (logo_w < 32) {
        logo_w = (int)app->fb.width;
    }
    int logo_h = (int)app->fb.height / 2;
    if (logo_h < 32) {
        logo_h = (int)app->fb.height;
    }
    int logo_x = ((int)app->fb.width - logo_w) / 2;
    if (logo_x < 0) {
        logo_x = 0;
    }
    int logo_y = ((int)app->fb.height / 2) - (logo_h / 2);
    if (logo_y < 0) {
        logo_y = 0;
    }

    if (app->boot_logo.loaded) {
        app_draw_boot_logo(app, logo_x, logo_y, logo_w, logo_h);
    } else {
        const char *fallback = "AUDIOX";
        int tw = ui_text_width(fallback, 3);
        int tx = ((int)app->fb.width - tw) / 2;
        int ty = logo_y + ((logo_h - ui_text_height(3)) / 2);
        ui_draw_text(&app->fb, tx, ty, fallback, fb_pack_color(&app->fb, 230, 238, 248), 3);
    }

    if (status_line && status_line[0]) {
        int sw = ui_text_width(status_line, 1);
        int sx = ((int)app->fb.width - sw) / 2;
        int sy = logo_y + logo_h + 14;
        if (sy + ui_text_height(1) > (int)app->fb.height - 2) {
            sy = (int)app->fb.height - ui_text_height(1) - 2;
        }
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
    g_boot_status_app = &app;
    g_boot_status_active = 1;
    g_boot_last_error[0] = '\0';
    runtime_log_set_sink(ui_log_push_adapter, &app.log);

    run_temp_fs_shell();

    // TODO: Load graphics first so that we can show a boot logo while loading modules
    app_boot_status(&app, "Loading kernel modules...");
    printf("[INIT] Loading kernel modules...\n");
    if (load_modules_from_list(MODULE_LOAD_LIST_FILE) < 0) {
        printf("[INIT] [CRIT] Failed to load kernel modules.\n");
        goto emergency_halt;
    }

    printf("[INIT] Kernel modules loaded.\n");
    app_try_enable_ui_logging(&app);
    app_boot_status(&app, app.fb_ready ? "Framebuffer ready. Initializing services..." : "Framebuffer not ready yet. Continuing startup...");

    app_boot_status(&app, "Loading config store...");
    (void)config_store_ensure(&app.config,
                              NULL,
                              runtime_log_line_adapter,
                              NULL);

    app_boot_status(&app, "Applying runtime config...");
    printf("[INIT] Applying runtime config...\n");
    if (app_reload_runtime_config(&app, 0) < 0) {
        printf("[INIT] [CRIT] Failed to configure USB audio gadget.\n");
        goto emergency_halt;
    }

    int network_support = 1;

    app_boot_status(&app, "Setting up USB network gadget...");
    printf("[INIT] USB audio gadget configured.\n");
    printf("[INIT] Setting up USB network gadget...\n");
    if (setup_usb_network_gadget() < 0) {
        printf("[INIT] [WARN] Failed to configure USB network gadget. Continuing without network support.\n");
        network_support = 0;
    }

    app_boot_status(&app, "Binding USB gadget to controller...");
    printf("[INIT] Binding gadget to UDC...\n");
    if (write_sys_node(GADGET_UDC_NODE, GADGET_UDC_NAME) < 0) {
        goto emergency_halt;
    }

    if (network_support) {
        app_boot_status(&app, "Waiting for network gadget to become ready...");
        printf("[INIT] Waiting for network gadget to become ready...\n");
        for (int i = 0; i < 20 && !is_network_ready(); ++i) {
            usleep(500 * 1000);
        }
        if (!is_network_ready()) {
            printf("[INIT] [WARN] Network gadget did not become ready. Continuing without network support.\n");
            network_support = 0;
        } else {
            app_boot_status(&app, "Setting up network interface...");
            printf("[INIT] Setting up network interface...\n");
            if (setup_network_interface() < 0) {
                printf("[INIT] [WARN] Failed to set up network interface. Continuing without network support.\n");
                network_support = 0;
            } else {
                app_boot_status(&app, "Starting HTTP server...");
                if (http_server_start(&app.http,
                                      80,
                                      "/etc/www/index.html",
                                      app_http_trigger_soundboard_slot,
                                      &app,
                                      app_http_reload_config,
                                      &app) < 0) {
                    printf("[INIT] [WARN] HTTP server failed to start. Continuing without web UI.\n");
                } else {
                    app.http_ready = 1;
                }
            }
        }
    }

    sleep(1);
    printf("\n========================================\n");
    printf("             audiox ready\n");
    printf("========================================\n\n");

    app_boot_status(&app, "Enumerating input devices...");
    debug_input_devices();

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
    g_boot_status_active = 0;
    g_boot_status_app = NULL;

    app_redraw_mode_t redraw_mode = APP_REDRAW_FULL;

    while (1) {
        int prev_log_head = app.log.head;
        int prev_log_count = app.log.count;

        int conn_changed = 0;
        int midi_updated = midi_runtime_poll(&app.midi, ui_log_push_adapter, &app.log, &conn_changed);
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
            .config_mount_active = app.config.mount_active,
            .usb_playback_channels = app.runtime_cfg.usb_playback_channels,
            .usb_capture_channels = app.runtime_cfg.usb_capture_channels,
            .usb_sample_rate = app.runtime_cfg.usb_sample_rate,
            .usb_sample_size = app.runtime_cfg.usb_sample_size,
            .active_tab = app.active_tab,
            .log_filter_mask = app.log_filter_mask,
            .log = &app.log,
            .boot_logo = &app.boot_logo
        };
        ui_console_io_t io;

        if (app.fb_ready) {
            ui_console_interact(&app.fb,
                                app.touch_ready ? &app.touch : NULL,
                                app.touch_ready ? raw_x : NULL,
                                app.touch_ready ? raw_y : NULL,
                                points,
                                &app.cfg_touch_latch,
                                &app.cfg_reload_touch_latch,
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
                model.config_mount_active = app.config.mount_active;
                model.usb_playback_channels = app.runtime_cfg.usb_playback_channels;
                model.usb_capture_channels = app.runtime_cfg.usb_capture_channels;
                model.usb_sample_rate = app.runtime_cfg.usb_sample_rate;
                model.usb_sample_size = app.runtime_cfg.usb_sample_size;
                model.active_tab = app.active_tab;
                model.log_filter_mask = app.log_filter_mask;
                model.log = &app.log;
                model.boot_logo = &app.boot_logo;

                if (redraw_mode == APP_REDRAW_FULL) {
                    fb_begin_frame(&app.fb);
                    ui_console_frame(&app.fb,
                                     app.touch_ready ? &app.touch : NULL,
                                     app.touch_ready ? raw_x : NULL,
                                     app.touch_ready ? raw_y : NULL,
                                     points,
                                     &app.cfg_touch_latch,
                                     &app.cfg_reload_touch_latch,
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
            (void)ui_im_rising_edge(0, &app.cfg_reload_touch_latch);
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
    if (g_boot_status_app && g_boot_status_app->fb_ready) {
        if (g_boot_last_error[0]) {
            app_boot_status(g_boot_status_app, g_boot_last_error);
        } else {
            app_boot_status(g_boot_status_app, "Startup error. Check UI log for details.");
        }
    }
    printf("[INIT] Critical startup error. Halting.\n");
    while (1) {
        sleep(60);
    }
    return 1;
}