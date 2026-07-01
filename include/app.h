#ifndef APP_H
#define APP_H

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "audio_oss.h"
#include "config.h"
#include "fb_touch.h"
#include "midi_input.h"
#include "ui_app.h"

typedef struct app_ctx {
    audio_ctx_t audio;
    fb_ctx_t fb;
    touch_ctx_t touch;
    ui_log_t log;
    midi_ctx_t midi;
    config_ctx_t config;

    int voice_enabled[4];
    int fb_ready;
    int touch_ready;
    int touch_retry_ticks;
    int hold_grace_ticks[2];
    int midi_probe_ticks;
    int cfg_touch_latched;
    int periodic_sync_ticks;
    int mount_poll_ticks;
    int touch_range_retry_ticks;
} app_ctx_t;

typedef void (*runtime_log_fn_t)(void *ctx, const char *line);

static inline int app_set_current_thread_other(const char *tag) {
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    if (sched_setscheduler(0, SCHED_OTHER, &param) < 0) {
        printf("[APP] [WARN] Failed to set %s thread to SCHED_OTHER: %s\n", tag, strerror(errno));
        return -1;
    }
    return 0;
}

static inline int app_create_fifo_audio_thread(pthread_t *thread, void *(*entry)(void *), void *arg, int priority) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        printf("[APP] [WARN] pthread_attr_init failed for audio thread.\n");
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

static inline void *app_audio_thread_main(void *arg) {
    audio_ctx_t *audio = (audio_ctx_t *)arg;
    while (1) {
        audio_write_next(audio);
    }
    return NULL;
}

static inline void app_show_terminal_cursor(void) {
    printf("\033[?25h");
    fflush(stdout);
}

static inline void app_hide_terminal_cursor(void) {
    printf("\033[?25l");
    fflush(stdout);
}

static inline int app_init(app_ctx_t *app) {
    if (!app) return -1;

    memset(app, 0, sizeof(*app));

    ui_log_init(&app->log);
    midi_ctx_init(&app->midi);
    config_init(&app->config);

    app->fb_ready = (fb_open(&app->fb) == 0);
    app->touch_ready = (touch_open(&app->touch) == 0);

    if (!app->touch_ready) {
        printf("[APP] [WARN] Touch control disabled. Audio will continue playing.\n");
    }

    if (audio_open(&app->audio) < 0) {
        return -1;
    }

    app_set_current_thread_other("main");

    pthread_t audio_thread;
    int audio_prio = 60;
    if (app_create_fifo_audio_thread(&audio_thread, app_audio_thread_main, &app->audio, audio_prio) != 0) {
        printf("[APP] [WARN] Failed to create FIFO audio thread, retrying with default scheduling.\n");
        if (pthread_create(&audio_thread, NULL, app_audio_thread_main, &app->audio) != 0) {
            printf("[APP] [CRIT] Failed to create audio thread.\n");
            audio_close(&app->audio);
            return -1;
        }

        struct sched_param fallback_param;
        memset(&fallback_param, 0, sizeof(fallback_param));
        fallback_param.sched_priority = audio_prio;
        if (pthread_setschedparam(audio_thread, SCHED_FIFO, &fallback_param) != 0) {
            printf("[APP] [WARN] Audio thread could not switch to SCHED_FIFO: %s\n", strerror(errno));
        }
    }

    for (int i = 0; i < 4; ++i) {
        app->voice_enabled[i] = audio_voice_is_enabled(&app->audio, i);
    }

    ui_log_push(&app->log, "UI READY");
    ui_log_push(&app->log, "WAITING FOR MIDI CONTROLLER...");
    ui_log_push(&app->log, app->config.mount_active ? "CFG FS MOUNTED" : "CFG FS UNMOUNTED");

    if (app->fb_ready) {
        ui_draw_scene(&app->fb, app->voice_enabled, &app->log, app->midi.connected, app->midi.dev_path,
                      app->config.enabled, app->config.mount_active);
    }

    return 0;
}

static inline void app_cleanup(app_ctx_t *app) {
    if (!app) return;

    midi_disconnect(&app->midi, ui_log_push_adapter, &app->log);
    audio_close(&app->audio);
    if (app->touch_ready) {
        touch_close(&app->touch);
    }
    if (app->fb_ready) {
        fb_close(&app->fb);
    }
}

static inline void app_run(app_ctx_t *app) {
    if (!app) return;

    const int hold_release_grace = 12;

    while (1) {
        int ui_dirty = 0;

        /* MIDI polling */
        if (++app->midi_probe_ticks >= 50 || app->midi.fd >= 0) {
            int conn_changed = 0;
            ui_dirty |= midi_poll(&app->midi, ui_log_push_adapter, &app->log, &conn_changed);
            if (conn_changed) {
                app->midi_probe_ticks = 0;
            }
        }

        /* Config mount polling */
        if (++app->mount_poll_ticks >= 2500) {
            app->mount_poll_ticks = 0;
            int mount_now = config_is_mount_active(CONFIG_MOUNT_POINT);
            if (mount_now != app->config.mount_active) {
                app->config.mount_active = mount_now;
                ui_log_push(&app->log, app->config.mount_active ? "CFG FS MOUNTED" : "CFG FS UNMOUNTED");
                ui_dirty = 1;
            }
        }

        /* Touch device recovery */
        if (!app->touch_ready) {
            if (++app->touch_retry_ticks >= 200) {
                app->touch_retry_ticks = 0;
                app->touch_ready = (touch_open(&app->touch) == 0);
                if (app->touch_ready) {
                    printf("[APP] Touch input became available during runtime.\n");
                    if (!app->touch.has_range) {
                        ui_log_push(&app->log, "TOUCH RANGE MISSING (FALLBACK)");
                    }
                }
            }
        } else {
            if (!app->touch.has_range && ++app->touch_range_retry_ticks >= 1000) {
                app->touch_range_retry_ticks = 0;
                touch_close(&app->touch);
                app->touch_ready = (touch_open(&app->touch) == 0);
                if (app->touch_ready && app->touch.has_range) {
                    ui_log_push(&app->log, "TOUCH RANGE RESTORED");
                } else if (app->touch_ready) {
                    ui_log_push(&app->log, "TOUCH RANGE STILL MISSING");
                }
            }

            if (app->fb_ready) {
                int raw_x[TOUCH_MAX_POINTS];
                int raw_y[TOUCH_MAX_POINTS];
                int touched_now[2] = {0, 0};
                int cfg_touched_now = 0;

                int points = touch_poll_points(&app->touch, raw_x, raw_y, TOUCH_MAX_POINTS);
                if (points < 0) {
                    ui_log_push(&app->log, "TOUCH READ FAILED; REOPENING");
                    touch_close(&app->touch);
                    app->touch_ready = 0;
                    continue;
                }

                if (points > 0) {
                    for (int i = 0; i < points; ++i) {
                        int sx = 0;
                        int sy = 0;
                        if (ui_touch_map_to_screen(&app->touch, &app->fb, raw_x[i], raw_y[i], &sx, &sy) == 0) {
                            int button = ui_hit_button(&app->fb, sx, sy);
                            if (button >= 0 && button < 2) {
                                touched_now[button] = 1;
                            } else if (ui_hit_config_toggle(&app->fb, sx, sy)) {
                                cfg_touched_now = 1;
                            }
                        }
                    }
                }

                /* Voice button handling */
                for (int i = 0; i < 2; ++i) {
                    if (touched_now[i]) {
                        app->hold_grace_ticks[i] = hold_release_grace;
                    } else if (app->hold_grace_ticks[i] > 0) {
                        --app->hold_grace_ticks[i];
                    }

                    int should_enable = (app->hold_grace_ticks[i] > 0) ? 1 : 0;

                    if (should_enable == app->voice_enabled[i]) {
                        continue;
                    }

                    app->voice_enabled[i] = should_enable;
                    audio_set_voice(&app->audio, i, should_enable);
                    ui_dirty = 1;
                }

                /* Config toggle handling */
                if (cfg_touched_now && !app->cfg_touch_latched) {
                    app->cfg_touch_latched = 1;
                    app->config.enabled = app->config.enabled ? 0 : 1;
                    if (config_write_flag(app->config.enabled) == 0) {
                        ui_log_push(&app->log, app->config.enabled ? "CONFIG TOGGLE: 1" : "CONFIG TOGGLE: 0");
                    } else {
                        ui_log_push(&app->log, "CONFIG WRITE FAILED");
                    }
                    ui_dirty = 1;
                }
                if (!cfg_touched_now) {
                    app->cfg_touch_latched = 0;
                }
            }
        }

        /* UI rendering */
        if (app->fb_ready && ui_dirty) {
            ui_draw_scene(&app->fb, app->voice_enabled, &app->log, app->midi.connected, app->midi.dev_path,
                          app->config.enabled, app->config.mount_active);
        }

        /* Periodic sync */
        if (++app->periodic_sync_ticks >= 5000) {
            sync();
            app->periodic_sync_ticks = 0;
        }

        usleep(2000);
    }
}

#endif
