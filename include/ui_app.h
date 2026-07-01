#ifndef UI_APP_H
#define UI_APP_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fb_touch.h"
#include "ui_fb.h"

#define TOUCH_MAX_POINTS 10

typedef struct ui_log {
    char lines[64][96];
    int count;
    int head;
} ui_log_t;

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
    uint32_t cfg_x;
    uint32_t cfg_y;
    uint32_t cfg_w;
    uint32_t cfg_h;
} ui_layout_t;

static inline void ui_log_init(ui_log_t *log) {
    if (!log) return;
    memset(log, 0, sizeof(*log));
}

static inline void ui_log_push(ui_log_t *log, const char *line) {
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

static inline int ui_log_get_line(const ui_log_t *log, int idx, const char **out) {
    if (!log || !out || idx < 0 || idx >= log->count) {
        return -1;
    }

    int start = (log->head - log->count + 64) % 64;
    int slot = (start + idx) % 64;
    *out = log->lines[slot];
    return 0;
}

/* Adapter function for runtime_log_fn_t style callbacks */
static inline void ui_log_push_adapter(void *ctx, const char *line) {
    ui_log_push((ui_log_t *)ctx, line);
}

static inline void ui_layout_compute(const fb_ctx_t *fb, ui_layout_t *layout) {
    if (!fb || !layout) return;

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

    int cfg_w = (int)layout->btn_w / 2;
    if (cfg_w < 80) {
        cfg_w = 80;
    }
    int cfg_h = ui_text_height(1) + 12;
    if (cfg_h < 20) {
        cfg_h = 20;
    }

    int cfg_x = (int)layout->log_x + (int)layout->log_w - cfg_w - 8;
    int cfg_y = (int)layout->log_y + 6;
    int cfg_min_x = (int)layout->log_x + 8;
    int cfg_min_y = (int)layout->log_y + 6;

    if (cfg_x < cfg_min_x) {
        cfg_x = cfg_min_x;
    }
    if (cfg_y < cfg_min_y) {
        cfg_y = cfg_min_y;
    }

    layout->cfg_w = (uint32_t)cfg_w;
    layout->cfg_h = (uint32_t)cfg_h;
    layout->cfg_x = (uint32_t)cfg_x;
    layout->cfg_y = (uint32_t)cfg_y;
}

static inline int ui_touch_map_to_screen(const touch_ctx_t *touch, const fb_ctx_t *fb, int raw_x, int raw_y, int *screen_x, int *screen_y) {
    if (!fb || fb->width == 0 || fb->height == 0 || !screen_x || !screen_y) {
        return -1;
    }

    if (!touch) {
        return -1;
    }

    if (!touch->has_range) {
        int sx = raw_x;
        int sy = raw_y;

        if (sx < 0 || sx >= (int)fb->width * 2 || sy < 0 || sy >= (int)fb->height * 2) {
            double nx = (double)raw_x / 4095.0;
            double ny = (double)raw_y / 4095.0;
            if (nx < 0.0) nx = 0.0;
            if (nx > 1.0) nx = 1.0;
            if (ny < 0.0) ny = 0.0;
            if (ny > 1.0) ny = 1.0;
            sx = (int)(nx * (double)(fb->width - 1));
            sy = (int)(ny * (double)(fb->height - 1));
        }

        if (sx < 0) sx = 0;
        if (sx >= (int)fb->width) sx = (int)fb->width - 1;
        if (sy < 0) sy = 0;
        if (sy >= (int)fb->height) sy = (int)fb->height - 1;

        *screen_x = sx;
        *screen_y = sy;
        return 0;
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

static inline int ui_hit_button(const fb_ctx_t *fb, int x, int y) {
    if (!fb) {
        return -1;
    }

    ui_layout_t layout;
    ui_layout_compute(fb, &layout);

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

static inline int ui_hit_config_toggle(const fb_ctx_t *fb, int x, int y) {
    if (!fb) {
        return 0;
    }

    ui_layout_t layout;
    ui_layout_compute(fb, &layout);

    int x0 = (int)layout.cfg_x;
    int y0 = (int)layout.cfg_y;
    int x1 = x0 + (int)layout.cfg_w;
    int y1 = y0 + (int)layout.cfg_h;

    if (x >= x0 && x < x1 && y >= y0 && y < y1) {
        return 1;
    }
    return 0;
}

static inline void ui_app_draw_voice_button(const fb_ctx_t *fb, int idx, int enabled) {
    if (!fb || !fb->map || idx < 0 || idx >= 2) {
        return;
    }

    ui_layout_t layout;
    ui_layout_compute(fb, &layout);

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

static inline void ui_draw_log_panel(const fb_ctx_t *fb,
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

static inline void ui_draw_scene(const fb_ctx_t *fb,
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
    ui_layout_compute(fb, &layout);

    uint32_t bg = fb_pack_color(fb, 8, 12, 16);
    ui_fill_rect(fb, 0, 0, fb->width, fb->height, bg);

    uint32_t title_color = fb_pack_color(fb, 222, 230, 240);
    int title_x = (int)(layout.margin);
    int title_y = (int)(layout.margin / 2);
    ui_draw_text(fb, title_x, title_y, "TOUCH VOICES + MIDI LOG", title_color, 2);

    for (int idx = 0; idx < 2; ++idx) {
        ui_app_draw_voice_button(fb, idx, enabled[idx]);
    }

    ui_draw_log_panel(fb, &layout, log, midi_connected, midi_dev, config_enabled, config_mount_active);
}

#endif
