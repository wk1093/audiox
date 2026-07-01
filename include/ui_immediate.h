#ifndef UI_IMMEDIATE_H
#define UI_IMMEDIATE_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fb_touch.h"
#include "ui_fb.h"

typedef struct ui_rect {
    int x;
    int y;
    int w;
    int h;
} ui_rect_t;

typedef struct ui_touch_latch {
    int was_down;
} ui_touch_latch_t;

typedef struct ui_im_ctx {
    const fb_ctx_t *fb;
    const touch_ctx_t *touch;
    int point_count;
    int sx[TOUCH_MAX_POINTS];
    int sy[TOUCH_MAX_POINTS];
} ui_im_ctx_t;

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

static inline int ui_touch_map_to_screen(const touch_ctx_t *touch,
                                         const fb_ctx_t *fb,
                                         int raw_x,
                                         int raw_y,
                                         int *screen_x,
                                         int *screen_y) {
    if (!touch || !fb || fb->width == 0 || fb->height == 0 || !screen_x || !screen_y) {
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

static inline void ui_im_begin(ui_im_ctx_t *ui,
                               const fb_ctx_t *fb,
                               const touch_ctx_t *touch,
                               const int *raw_x,
                               const int *raw_y,
                               int raw_points) {
    if (!ui) {
        return;
    }

    memset(ui, 0, sizeof(*ui));
    ui->fb = fb;
    ui->touch = touch;

    if (!fb || !touch || raw_points <= 0 || !raw_x || !raw_y) {
        return;
    }

    int points = raw_points;
    if (points > TOUCH_MAX_POINTS) {
        points = TOUCH_MAX_POINTS;
    }

    int out_idx = 0;
    for (int i = 0; i < points; ++i) {
        int sx = 0;
        int sy = 0;
        if (ui_touch_map_to_screen(touch, fb, raw_x[i], raw_y[i], &sx, &sy) == 0 && out_idx < TOUCH_MAX_POINTS) {
            ui->sx[out_idx] = sx;
            ui->sy[out_idx] = sy;
            ++out_idx;
        }
    }
    ui->point_count = out_idx;
}

static inline ui_rect_t ui_rect_make(int x, int y, int w, int h) {
    ui_rect_t r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

static inline int ui_rect_contains(const ui_rect_t *r, int x, int y) {
    if (!r || r->w <= 0 || r->h <= 0) {
        return 0;
    }
    return x >= r->x && x < (r->x + r->w) && y >= r->y && y < (r->y + r->h);
}

static inline int ui_im_touching_rect(const ui_im_ctx_t *ui, const ui_rect_t *r) {
    if (!ui || !r) {
        return 0;
    }

    for (int i = 0; i < ui->point_count; ++i) {
        if (ui_rect_contains(r, ui->sx[i], ui->sy[i])) {
            return 1;
        }
    }
    return 0;
}

static inline int ui_im_rising_edge(int down_now, ui_touch_latch_t *latch) {
    if (!latch) {
        return down_now ? 1 : 0;
    }

    int rising = down_now && !latch->was_down;
    latch->was_down = down_now ? 1 : 0;
    return rising;
}

static inline int ui_im_button(const ui_im_ctx_t *ui,
                               const ui_rect_t *r,
                               const char *label,
                               const ui_button_theme_t *theme,
                               int active,
                               uint32_t text_scale) {
    if (!ui || !ui->fb || !r || !theme) {
        return 0;
    }

    ui_draw_button(ui->fb,
                   (uint32_t)r->x,
                   (uint32_t)r->y,
                   (uint32_t)r->w,
                   (uint32_t)r->h,
                   label,
                   theme,
                   active,
                   text_scale);

    return ui_im_touching_rect(ui, r);
}

static inline int ui_im_toggle_button(const ui_im_ctx_t *ui,
                                      const ui_rect_t *r,
                                      const char *label,
                                      const ui_button_theme_t *theme,
                                      int active,
                                      uint32_t text_scale,
                                      ui_touch_latch_t *latch) {
    int down = ui_im_button(ui, r, label, theme, active, text_scale);
    return ui_im_rising_edge(down, latch);
}

static inline void ui_layout_compute_default(const fb_ctx_t *fb, ui_layout_t *layout) {
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

static inline ui_rect_t ui_layout_voice_button_rect(const ui_layout_t *layout, int idx) {
    int col = idx % 2;
    int bx = (int)layout->margin + col * (int)(layout->btn_w + layout->gap);
    int by = (int)layout->margin + (int)layout->title_h;
    return ui_rect_make(bx, by, (int)layout->btn_w, (int)layout->btn_h);
}

static inline ui_rect_t ui_layout_config_rect(const ui_layout_t *layout) {
    return ui_rect_make((int)layout->cfg_x,
                        (int)layout->cfg_y,
                        (int)layout->cfg_w,
                        (int)layout->cfg_h);
}

#endif
