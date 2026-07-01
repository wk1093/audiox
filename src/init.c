#include <stdio.h>
#include <stdlib.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <stdarg.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <unistd.h>

#include "audio_oss.h"
#include "fb_touch.h"
#include "init_defs.h"
#include "init_helpers.h"
#include "midi_input.h"
#include "ui_immediate.h"
#include "usb_gadget.h"

#ifndef DEBUG_SHELL
#define DEBUG_SHELL 0
#endif

#define CONFIG_MOUNT_POINT "/raspi_audio"
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

#if DEBUG_SHELL
static void fs_shell_help(void) {
    printf("\n");
    printf("Temporary Initramfs FS Shell\n");
    printf("  help                 Show this help\n");
    printf("  pwd                  Print current directory\n");
    printf("  cd <dir>             Change directory\n");
    printf("  ls [dir]             List directory entries\n");
    printf("  mounts               Show mounted filesystems\n");
    printf("  cat <file>           Print text file\n");
    printf("  boot | continue      Exit shell and continue normal init\n");
    printf("  halt                 Stop here forever\n");
    printf("\n");
}

static const int ls_linecap = 8;

static int fs_shell_ls(const char *path) {
    const char *target = (path && path[0] != '\0') ? path : ".";
    DIR *dir = opendir(target);
    if (!dir) {
        printf("[FS-SHELL] ls failed for '%s': %s\n", target, strerror(errno));
        return -1;
    }

    struct dirent *entry = NULL;
    int i = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (entry->d_type == DT_DIR) {
            printf("%s/ ", entry->d_name);
            if (++i % ls_linecap == 0) printf("\n");
        } else {
            printf("%s ", entry->d_name);
            if (++i % ls_linecap == 0) printf("\n");
        }
    }

    closedir(dir);
    return 0;
}

static int fs_shell_mounts(void) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        printf("[FS-SHELL] could not open /proc/mounts: %s\n", strerror(errno));
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char src[160];
        char dst[160];
        char fstype[64];
        if (sscanf(line, "%159s %159s %63s", src, dst, fstype) == 3) {
            printf("%-24s %-24s %s\n", src, dst, fstype);
        }
    }

    fclose(fp);
    return 0;
}

static int fs_shell_cat(const char *path) {
    if (!path || path[0] == '\0') {
        printf("[FS-SHELL] usage: cat <file>\n");
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("[FS-SHELL] cat failed for '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        fputs(line, stdout);
    }
    fclose(fp);
    return 0;
}

static void run_temp_fs_shell(void) {
    if (!isatty(STDIN_FILENO)) {
        printf("[INIT] stdin is not a tty; skipping temporary FS shell.\n");
        return;
    }

    printf("\n[INIT] Entering temporary filesystem shell. Type 'boot' to continue init.\n");
    fs_shell_help();

    char cwd[PATH_MAX];
    char line[512];

    while (1) {
        if (!getcwd(cwd, sizeof(cwd))) {
            snprintf(cwd, sizeof(cwd), "?");
        }
        printf("fs:%s# ", cwd);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n[INIT] EOF on shell input, continuing init.\n");
            break;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            --len;
        }

        char *cmd = strtok(line, " \t");
        if (!cmd) {
            continue;
        }

        char *arg = strtok(NULL, "");
        if (arg) {
            while (*arg == ' ' || *arg == '\t') {
                ++arg;
            }
        }

        if (strcmp(cmd, "help") == 0) {
            fs_shell_help();
            continue;
        }

        if (strcmp(cmd, "pwd") == 0) {
            if (getcwd(cwd, sizeof(cwd))) {
                printf("%s\n", cwd);
            } else {
                printf("[FS-SHELL] pwd failed: %s\n", strerror(errno));
            }
            continue;
        }

        if (strcmp(cmd, "cd") == 0) {
            const char *target = (arg && arg[0] != '\0') ? arg : "/";
            if (chdir(target) < 0) {
                printf("[FS-SHELL] cd failed for '%s': %s\n", target, strerror(errno));
            }
            continue;
        }

        if (strcmp(cmd, "ls") == 0) {
            (void)fs_shell_ls(arg);
            continue;
        }

        if (strcmp(cmd, "mounts") == 0) {
            (void)fs_shell_mounts();
            continue;
        }

        if (strcmp(cmd, "cat") == 0) {
            (void)fs_shell_cat(arg);
            continue;
        }

        if (strcmp(cmd, "boot") == 0 || strcmp(cmd, "continue") == 0) {
            break;
        }

        if (strcmp(cmd, "halt") == 0) {
            printf("[INIT] Halt requested from FS shell.\n");
            while (1) {
                sleep(60);
            }
        }

        printf("[FS-SHELL] unknown command '%s' (type 'help')\n", cmd);
    }

    if (chdir("/") < 0) {
        printf("[INIT] [WARN] Failed to restore cwd to '/': %s\n", strerror(errno));
    }
    printf("[INIT] Leaving temporary filesystem shell.\n\n");
}

#else

static void run_temp_fs_shell(void) {
    printf("[INIT] DEBUG_SHELL disabled; skipping temporary filesystem shell.\n");
}

#endif

static int path_exists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0) ? 1 : 0;
}

static int is_mount_active(const char *mount_point) {
    if (!mount_point) {
        return 0;
    }

    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        return 0;
    }

    char line[512];
    int active = 0;
    while (fgets(line, sizeof(line), fp) != NULL) {
        char src[160];
        char dst[160];
        char fstype[64];
        if (sscanf(line, "%159s %159s %63s", src, dst, fstype) == 3) {
            if (strcmp(dst, mount_point) == 0) {
                active = 1;
                break;
            }
        }
    }
    fclose(fp);
    return active;
}

static int mount_config_partition(void) {
    if (mkdir(CONFIG_MOUNT_POINT, 0777) < 0 && errno != EEXIST) {
        runtime_logf("[INIT] [WARN] Failed to create %s: %s", CONFIG_MOUNT_POINT, strerror(errno));
        return -1;
    }

    if (is_mount_active(CONFIG_MOUNT_POINT)) {
        return 0;
    }

    if (!path_exists(CONFIG_DEVICE_PATH)) {
        runtime_logf("[INIT] [WARN] %s not present; config persistence not mounted.", CONFIG_DEVICE_PATH);
        return -1;
    }

    if (mount(CONFIG_DEVICE_PATH, CONFIG_MOUNT_POINT, "ext4", MS_NOATIME, "") < 0) {
        runtime_logf("[INIT] [WARN] Failed to mount %s at %s: %s",
                     CONFIG_DEVICE_PATH,
                     CONFIG_MOUNT_POINT,
                     strerror(errno));
        return -1;
    }

    runtime_logf("[INIT] Mounted %s at %s for persistent config storage.", CONFIG_DEVICE_PATH, CONFIG_MOUNT_POINT);
    sync();
    return 0;
}

static int write_config_flag(int enabled) {
    FILE *fp = fopen(CONFIG_FILE_PATH, "w");
    if (!fp) {
        runtime_logf("[INIT] [WARN] Failed to open %s for write: %s", CONFIG_FILE_PATH, strerror(errno));
        return -1;
    }

    if (fprintf(fp, "%d\n", enabled ? 1 : 0) < 0) {
        runtime_logf("[INIT] [WARN] Failed to write %s: %s", CONFIG_FILE_PATH, strerror(errno));
        fclose(fp);
        return -1;
    }

    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    sync();
    return 0;
}

static int read_config_flag(void) {
    FILE *fp = fopen(CONFIG_FILE_PATH, "r");
    if (!fp) {
        return 0;
    }

    int c = fgetc(fp);
    fclose(fp);
    return (c == '1') ? 1 : 0;
}

static int ensure_config_file_and_state(int *enabled_out) {
    if (enabled_out) {
        *enabled_out = 0;
    }

    (void)mount_config_partition();

    if (!path_exists(CONFIG_FILE_PATH)) {
        if (write_config_flag(0) < 0) {
            runtime_logf("[INIT] [WARN] Could not create default %s.", CONFIG_FILE_PATH);
            return -1;
        }
        runtime_logf("[INIT] Created default persistent config at %s", CONFIG_FILE_PATH);
    }

    if (enabled_out) {
        *enabled_out = read_config_flag();
    }

    return 0;
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

static void ui_log_push_adapter(void *ctx, const char *line) {
    ui_log_push((ui_log_t *)ctx, line);
}

typedef struct ui_interaction {
    int voice_touched[2];
    int config_toggled;
} ui_interaction_t;

static void fb_draw_log_panel(const fb_ctx_t *fb,
                              const ui_layout_t *layout,
                              const ui_log_t *log,
                              int midi_connected,
                              const char *midi_dev,
                              int config_enabled,
                              int config_mount_active) {
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

    ui_draw_text(fb,
                 text_x,
                 text_y + ui_text_height(1) + 2,
                 config_mount_active ? "CFG FS: MOUNTED" : "CFG FS: UNMOUNTED",
                 config_mount_active ? status_ok : status_wait,
                 1);

    ui_button_theme_t cfg_theme;
    memset(&cfg_theme, 0, sizeof(cfg_theme));
    cfg_theme.fill = fb_pack_color(fb, 42, 46, 52);
    cfg_theme.fill_active = fb_pack_color(fb, 24, 146, 74);
    cfg_theme.border = fb_pack_color(fb, 120, 126, 134);
    cfg_theme.border_active = fb_pack_color(fb, 210, 246, 224);
    cfg_theme.text = fb_pack_color(fb, 224, 232, 242);
    cfg_theme.text_active = fb_pack_color(fb, 248, 252, 255);

    char cfg_label[24];
    snprintf(cfg_label, sizeof(cfg_label), "CFG %s", config_enabled ? "ON" : "OFF");
    ui_draw_button(fb,
                   layout->cfg_x,
                   layout->cfg_y,
                   layout->cfg_w,
                   layout->cfg_h,
                   cfg_label,
                   &cfg_theme,
                   config_enabled,
                   1);

    int line_h = ui_text_height(1) + 2;
    int first_line_y = (int)layout->cfg_y + (int)layout->cfg_h + 8;
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

static void ui_sample_interaction(const fb_ctx_t *fb,
                                  const touch_ctx_t *touch,
                                  const int *raw_x,
                                  const int *raw_y,
                                  int points,
                                  ui_touch_latch_t *cfg_latch,
                                  ui_interaction_t *out) {
    memset(out, 0, sizeof(*out));
    if (!fb || !touch || points < 0) {
        (void)ui_im_rising_edge(0, cfg_latch);
        return;
    }

    ui_layout_t layout;
    ui_layout_compute_default(fb, &layout);

    ui_im_ctx_t ui;
    ui_im_begin(&ui, fb, touch, raw_x, raw_y, points);

    for (int i = 0; i < 2; ++i) {
        ui_rect_t voice_rect = ui_layout_voice_button_rect(&layout, i);
        out->voice_touched[i] = ui_im_touching_rect(&ui, &voice_rect);
    }

    ui_rect_t cfg_rect = ui_layout_config_rect(&layout);
    int cfg_down = ui_im_touching_rect(&ui, &cfg_rect);
    out->config_toggled = ui_im_rising_edge(cfg_down, cfg_latch);
}

static void fb_draw_scene(const fb_ctx_t *fb,
                          const int *enabled,
                          const ui_log_t *log,
                          int midi_connected,
                          const char *midi_dev,
                          int config_enabled,
                          int config_mount_active) {
    if (!fb || !fb->map || !enabled || !log) {
        return;
    }

    ui_layout_t layout;
    ui_layout_compute_default(fb, &layout);

    uint32_t bg = fb_pack_color(fb, 8, 12, 16);
    ui_fill_rect(fb, 0, 0, fb->width, fb->height, bg);

    uint32_t title_color = fb_pack_color(fb, 222, 230, 240);
    int title_x = (int)(layout.margin);
    int title_y = (int)(layout.margin / 2);
    ui_draw_text(fb, title_x, title_y, "TOUCH VOICES + MIDI LOG", title_color, 2);

    static const char *labels[2] = {"VOICE 1", "VOICE 2"};
    uint32_t off_fill = fb_pack_color(fb, 42, 46, 52);
    uint32_t on_fill[2] = {
        fb_pack_color(fb, 24, 146, 74),
        fb_pack_color(fb, 20, 96, 178)
    };
    uint32_t off_border = fb_pack_color(fb, 120, 126, 134);
    uint32_t on_border = fb_pack_color(fb, 210, 246, 224);

    for (int idx = 0; idx < 2; ++idx) {
        ui_button_theme_t voice_theme;
        memset(&voice_theme, 0, sizeof(voice_theme));
        voice_theme.fill = off_fill;
        voice_theme.fill_active = on_fill[idx];
        voice_theme.border = off_border;
        voice_theme.border_active = on_border;
        voice_theme.text = fb_pack_color(fb, 210, 216, 226);
        voice_theme.text_active = fb_pack_color(fb, 248, 252, 255);

        ui_rect_t r = ui_layout_voice_button_rect(&layout, idx);
        ui_draw_button(fb,
                       (uint32_t)r.x,
                       (uint32_t)r.y,
                       (uint32_t)r.w,
                       (uint32_t)r.h,
                       labels[idx],
                       &voice_theme,
                       enabled[idx],
                       2);
    }

    fb_draw_log_panel(fb, &layout, log, midi_connected, midi_dev, config_enabled, config_mount_active);
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

    int config_enabled = 0;

    run_temp_fs_shell();

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
    runtime_log_set_sink(ui_log_push_adapter, &log);

    (void)ensure_config_file_and_state(&config_enabled);
    int config_mount_active = is_mount_active(CONFIG_MOUNT_POINT);

    ui_log_push(&log, "UI READY");
    ui_log_push(&log, "WAITING FOR MIDI CONTROLLER...");
    ui_log_push(&log, config_mount_active ? "CFG FS MOUNTED" : "CFG FS UNMOUNTED");

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
        fb_draw_scene(&fb, voice_enabled, &log, midi.connected, midi.dev_path, config_enabled, config_mount_active);
    }

    int touch_ready = (touch_open(&touch) == 0);
    if (!touch_ready) {
        printf("[INIT] [WARN] Touch control disabled. Audio will continue playing.\n");
    }

    int touch_retry_ticks = 0;
    int hold_grace_ticks[2] = {0, 0};
    const int hold_release_grace = 12;
    int midi_probe_ticks = 0;
    ui_touch_latch_t cfg_touch_latch;
    memset(&cfg_touch_latch, 0, sizeof(cfg_touch_latch));
    int periodic_sync_ticks = 0;
    int mount_poll_ticks = 0;
    int touch_range_retry_ticks = 0;

    while (1) {
        int ui_dirty = 0;

        if (++midi_probe_ticks >= 50 || midi.fd >= 0) {
            int conn_changed = 0;
            ui_dirty |= midi_poll(&midi, ui_log_push_adapter, &log, &conn_changed);
            if (conn_changed) {
                midi_probe_ticks = 0;
            }
        }

        if (++mount_poll_ticks >= 2500) {
            mount_poll_ticks = 0;
            int mount_now = is_mount_active(CONFIG_MOUNT_POINT);
            if (mount_now != config_mount_active) {
                config_mount_active = mount_now;
                ui_log_push(&log, config_mount_active ? "CFG FS MOUNTED" : "CFG FS UNMOUNTED");
                ui_dirty = 1;
            }
        }

        if (!touch_ready) {
            if (++touch_retry_ticks >= 200) {
                touch_retry_ticks = 0;
                touch_ready = (touch_open(&touch) == 0);
                if (touch_ready) {
                    printf("[INIT] Touch input became available during runtime.\n");
                    if (!touch.has_range) {
                        ui_log_push(&log, "TOUCH RANGE MISSING (FALLBACK)");
                    }
                }
            }
            (void)ui_im_rising_edge(0, &cfg_touch_latch);
        } else {
            if (!touch.has_range && ++touch_range_retry_ticks >= 1000) {
                touch_range_retry_ticks = 0;
                touch_close(&touch);
                touch_ready = (touch_open(&touch) == 0);
                if (touch_ready && touch.has_range) {
                    ui_log_push(&log, "TOUCH RANGE RESTORED");
                } else if (touch_ready) {
                    ui_log_push(&log, "TOUCH RANGE STILL MISSING");
                }
            }

            if (fb_ready) {
                int raw_x[TOUCH_MAX_POINTS];
                int raw_y[TOUCH_MAX_POINTS];
                ui_interaction_t interaction;

                int points = touch_poll_points(&touch, raw_x, raw_y, TOUCH_MAX_POINTS);
                if (points < 0) {
                    ui_log_push(&log, "TOUCH READ FAILED; REOPENING");
                    touch_close(&touch);
                    touch_ready = 0;
                    continue;
                }

                ui_sample_interaction(&fb,
                                      &touch,
                                      raw_x,
                                      raw_y,
                                      points,
                                      &cfg_touch_latch,
                                      &interaction);

                for (int i = 0; i < 2; ++i) {
                    if (interaction.voice_touched[i]) {
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

                if (interaction.config_toggled) {
                    config_enabled = config_enabled ? 0 : 1;
                    if (write_config_flag(config_enabled) == 0) {
                        ui_log_push(&log, config_enabled ? "CONFIG TOGGLE: 1" : "CONFIG TOGGLE: 0");
                    } else {
                        ui_log_push(&log, "CONFIG WRITE FAILED");
                    }
                    ui_dirty = 1;
                }
            }
        }

        if (fb_ready && ui_dirty) {
            fb_draw_scene(&fb, voice_enabled, &log, midi.connected, midi.dev_path, config_enabled, config_mount_active);
        }

        if (++periodic_sync_ticks >= 5000) {
            sync();
            periodic_sync_ticks = 0;
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