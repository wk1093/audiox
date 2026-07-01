#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

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

static runtime_log_fn_t g_runtime_log_fn = NULL;
static void *g_runtime_log_ctx = NULL;

static void runtime_log_set_sink(runtime_log_fn_t fn, void *ctx) {
    g_runtime_log_fn = fn;
    g_runtime_log_ctx = ctx;
}

static void runtime_logf(const char *fmt, ...) {
    char line[192];

    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);

    printf("%s\n", line);
    if (g_runtime_log_fn) {
        g_runtime_log_fn(g_runtime_log_ctx, line);
    }
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

typedef struct app_state {
    audio_ctx_t audio;
    fb_ctx_t fb;
    touch_ctx_t touch;
    ui_log_t log;
    midi_runtime_t midi;
    config_store_t config;
    int config_enabled;
    int fb_ready;
    int touch_ready;
    int voice_enabled[4];
    int hold_grace_ticks[2];
    ui_touch_latch_t cfg_touch_latch;
} app_state_t;

typedef struct app_ticks {
    tick_task_t touch_retry;
    tick_task_t touch_range_retry;
    tick_task_t mount_poll;
    tick_task_t periodic_sync;
} app_ticks_t;

static void app_ticks_init(app_ticks_t *ticks) {
    tick_task_init(&ticks->touch_retry, 200);
    tick_task_init(&ticks->touch_range_retry, 1000);
    tick_task_init(&ticks->mount_poll, 2500);
    tick_task_init(&ticks->periodic_sync, 5000);
}

static void app_state_init(app_state_t *app) {
    memset(app, 0, sizeof(*app));
    ui_log_init(&app->log);
    midi_runtime_init(&app->midi, 50);
    config_store_init(&app->config, CONFIG_MOUNT_POINT, CONFIG_DEVICE_PATH, CONFIG_FILE_PATH);
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
                        const ui_console_io_t *io,
                        int hold_release_grace) {
    int changed = 0;

    for (int i = 0; i < 2; ++i) {
        if (io->voice_touch[i]) {
            app->hold_grace_ticks[i] = hold_release_grace;
        } else if (app->hold_grace_ticks[i] > 0) {
            --app->hold_grace_ticks[i];
        }

        int should_enable = (app->hold_grace_ticks[i] > 0) ? 1 : 0;
        if (should_enable != app->voice_enabled[i]) {
            app->voice_enabled[i] = should_enable;
            audio_set_voice(&app->audio, i, should_enable);
            changed = 1;
        }
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

    return changed;
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

    run_temp_fs_shell();

    printf("[INIT] Loading kernel modules...\n");
    if (load_modules_from_list(MODULE_LOAD_LIST_FILE) < 0) {
        printf("[INIT] [CRIT] Failed to load kernel modules.\n");
        goto emergency_halt;
    }
    sleep(3);
    printf("[INIT] Kernel modules loaded.\n");

    printf("[INIT] Setting up USB audio gadget...\n");
    if (setup_usb_audio_gadget() < 0) {
        printf("[INIT] [CRIT] Failed to configure USB audio gadget.\n");
        goto emergency_halt;
    }

    printf("[INIT] Binding gadget to UDC...\n");
    if (write_sys_node(GADGET_UDC_NODE, GADGET_UDC_NAME) < 0) {
        goto emergency_halt;
    }

    sleep(1);

    printf("\n========================================\n");
    printf("             audiox ready\n");
    printf("========================================\n\n");

    debug_input_devices();

    app_state_t app;
    app_ticks_t ticks;
    app_state_init(&app);
    app_ticks_init(&ticks);

    runtime_log_set_sink(ui_log_push_adapter, &app.log);

    (void)config_store_ensure(&app.config,
                              &app.config_enabled,
                              runtime_log_line_adapter,
                              NULL);

    ui_log_push(&app.log, "UI READY");
    ui_log_push(&app.log, "WAITING FOR MIDI CONTROLLER...");
    ui_log_push(&app.log, app.config.mount_active ? "CFG FS MOUNTED" : "CFG FS UNMOUNTED");

    if (app_open_audio_thread(&app) < 0) {
        goto emergency_halt;
    }

    app.fb_ready = (fb_open(&app.fb) == 0);
    app.voice_enabled[0] = audio_voice_is_enabled(&app.audio, 0);
    app.voice_enabled[1] = audio_voice_is_enabled(&app.audio, 1);
    app.voice_enabled[2] = audio_voice_is_enabled(&app.audio, 2);
    app.voice_enabled[3] = audio_voice_is_enabled(&app.audio, 3);

    app.touch_ready = (touch_open(&app.touch) == 0);
    if (!app.touch_ready) {
        printf("[INIT] [WARN] Touch control disabled. Audio will continue playing.\n");
    }

    const int hold_release_grace = 12;
    int render_dirty = 1;

    while (1) {
        int prev_log_head = app.log.head;
        int prev_log_count = app.log.count;

        int conn_changed = 0;
        int midi_updated = midi_runtime_poll(&app.midi, ui_log_push_adapter, &app.log, &conn_changed);
        if (midi_updated || conn_changed) {
            render_dirty = 1;
        }

        if (tick_task_due(&ticks.mount_poll)) {
            int mount_before = app.config.mount_active;
            int mount_after = config_store_refresh_mount_state(&app.config);
            if (mount_before != mount_after) {
                ui_log_push(&app.log, mount_after ? "CFG FS MOUNTED" : "CFG FS UNMOUNTED");
                render_dirty = 1;
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
            .midi_connected = app.midi.ctx.connected,
            .midi_dev = app.midi.ctx.dev_path,
            .config_enabled = app.config_enabled,
            .config_mount_active = app.config.mount_active,
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
                                &model,
                                &io);
            if (app_apply_ui(&app, &io, hold_release_grace)) {
                render_dirty = 1;
            }

            if (app.log.head != prev_log_head || app.log.count != prev_log_count) {
                render_dirty = 1;
            }

            if (render_dirty) {
                model.voice_enabled = app.voice_enabled;
                model.midi_connected = app.midi.ctx.connected;
                model.midi_dev = app.midi.ctx.dev_path;
                model.config_enabled = app.config_enabled;
                model.config_mount_active = app.config.mount_active;
                model.log = &app.log;

                fb_begin_frame(&app.fb);

                ui_console_frame(&app.fb,
                                 app.touch_ready ? &app.touch : NULL,
                                 app.touch_ready ? raw_x : NULL,
                                 app.touch_ready ? raw_y : NULL,
                                 points,
                                 &app.cfg_touch_latch,
                                 &model,
                                 NULL);
                (void)fb_present(&app.fb);
                render_dirty = 0;
            }
        } else {
            (void)ui_im_rising_edge(0, &app.cfg_touch_latch);
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