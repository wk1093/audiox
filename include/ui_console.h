#ifndef UI_CONSOLE_H
#define UI_CONSOLE_H

#include <string.h>

#include "ui_immediate.h"

typedef struct ui_console_theme {
    ui_button_theme_t voice_theme[2];
    ui_button_theme_t cfg_theme;
    uint32_t bg;
    uint32_t title;
    uint32_t panel_border;
    uint32_t panel_fill;
    uint32_t header;
    uint32_t text;
    uint32_t status_ok;
    uint32_t status_wait;
} ui_console_theme_t;

typedef struct ui_console_model {
    const int *voice_enabled;
    int midi_connected;
    const char *midi_dev;
    int config_enabled;
    int config_mount_active;
    const ui_log_t *log;
} ui_console_model_t;

typedef struct ui_console_io {
    int voice_touch[2];
    int config_toggled;
} ui_console_io_t;

static inline void ui_console_interact(const fb_ctx_t *fb,
                                       const touch_ctx_t *touch,
                                       const int *raw_x,
                                       const int *raw_y,
                                       int points,
                                       ui_touch_latch_t *cfg_latch,
                                       const ui_console_model_t *model,
                                       ui_console_io_t *io) {
    if (io) {
        memset(io, 0, sizeof(*io));
    }

    if (!fb || !fb->map || !model || !model->voice_enabled) {
        if (cfg_latch) {
            (void)ui_im_rising_edge(0, cfg_latch);
        }
        return;
    }

    ui_layout_t layout;
    ui_layout_compute_default(fb, &layout);

    ui_im_ctx_t ui;
    ui_im_begin(&ui, fb, touch, raw_x, raw_y, points);

    for (int idx = 0; idx < 2; ++idx) {
        ui_rect_t vr = ui_layout_voice_button_rect(&layout, idx);
        int touched = ui_im_touching_rect(&ui, &vr);
        if (io) {
            io->voice_touch[idx] = touched;
        }
    }

    ui_rect_t cfg_rect = ui_layout_config_rect(&layout);
    int toggled = ui_im_rising_edge(ui_im_touching_rect(&ui, &cfg_rect), cfg_latch);
    if (io) {
        io->config_toggled = toggled;
    }
}

static inline void ui_console_theme_default(const fb_ctx_t *fb, ui_console_theme_t *theme) {
    memset(theme, 0, sizeof(*theme));

    theme->bg = fb_pack_color(fb, 8, 12, 16);
    theme->title = fb_pack_color(fb, 222, 230, 240);

    theme->voice_theme[0].fill = fb_pack_color(fb, 42, 46, 52);
    theme->voice_theme[0].fill_active = fb_pack_color(fb, 24, 146, 74);
    theme->voice_theme[0].border = fb_pack_color(fb, 120, 126, 134);
    theme->voice_theme[0].border_active = fb_pack_color(fb, 210, 246, 224);
    theme->voice_theme[0].text = fb_pack_color(fb, 210, 216, 226);
    theme->voice_theme[0].text_active = fb_pack_color(fb, 248, 252, 255);

    theme->voice_theme[1] = theme->voice_theme[0];
    theme->voice_theme[1].fill_active = fb_pack_color(fb, 20, 96, 178);

    theme->cfg_theme.fill = fb_pack_color(fb, 42, 46, 52);
    theme->cfg_theme.fill_active = fb_pack_color(fb, 24, 146, 74);
    theme->cfg_theme.border = fb_pack_color(fb, 120, 126, 134);
    theme->cfg_theme.border_active = fb_pack_color(fb, 210, 246, 224);
    theme->cfg_theme.text = fb_pack_color(fb, 224, 232, 242);
    theme->cfg_theme.text_active = fb_pack_color(fb, 248, 252, 255);

    theme->panel_border = fb_pack_color(fb, 112, 122, 136);
    theme->panel_fill = fb_pack_color(fb, 16, 20, 24);
    theme->header = fb_pack_color(fb, 224, 232, 242);
    theme->text = fb_pack_color(fb, 182, 196, 212);
    theme->status_ok = fb_pack_color(fb, 128, 224, 148);
    theme->status_wait = fb_pack_color(fb, 236, 184, 96);
}

static inline void ui_console_draw_log_panel(const fb_ctx_t *fb,
                                             const ui_layout_t *layout,
                                             const ui_console_theme_t *theme,
                                             const ui_console_model_t *model) {
    if (!fb || !fb->map || !layout || !theme || !model || !model->log) {
        return;
    }

    ui_fill_rect(fb, layout->log_x, layout->log_y, layout->log_w, layout->log_h, theme->panel_border);
    if (layout->log_w > 4 && layout->log_h > 4) {
        ui_fill_rect(fb, layout->log_x + 2, layout->log_y + 2, layout->log_w - 4, layout->log_h - 4, theme->panel_fill);
    }

    int text_x = (int)layout->log_x + 8;
    int text_y = (int)layout->log_y + 8;
    ui_draw_text(fb, text_x, text_y, "MIDI LOG", theme->header, 1);

    char status_line[160];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(status_line,
             sizeof(status_line),
             "STATUS: %s%s%s",
             model->midi_connected ? "CONNECTED" : "WAITING",
             (model->midi_connected && model->midi_dev && model->midi_dev[0] != '\0') ? " " : "",
             (model->midi_connected && model->midi_dev && model->midi_dev[0] != '\0') ? model->midi_dev : "");
#pragma GCC diagnostic pop
    ui_draw_text(fb,
                 text_x + 72,
                 text_y,
                 status_line,
                 model->midi_connected ? theme->status_ok : theme->status_wait,
                 1);

    ui_draw_text(fb,
                 text_x,
                 text_y + ui_text_height(1) + 2,
                 model->config_mount_active ? "CFG FS: MOUNTED" : "CFG FS: UNMOUNTED",
                 model->config_mount_active ? theme->status_ok : theme->status_wait,
                 1);

    int line_h = ui_text_height(1) + 2;
    int first_line_y = (int)layout->cfg_y + (int)layout->cfg_h + 8;
    int max_lines = ((int)layout->log_h - (first_line_y - (int)layout->log_y) - 8) / line_h;
    if (max_lines < 1) {
        return;
    }

    int start_idx = model->log->count - max_lines;
    if (start_idx < 0) {
        start_idx = 0;
    }

    int row = 0;
    for (int i = start_idx; i < model->log->count; ++i) {
        const char *line = NULL;
        if (ui_log_get_line(model->log, i, &line) == 0 && line) {
            ui_draw_text(fb, text_x, first_line_y + (row * line_h), line, theme->text, 1);
        }
        ++row;
    }
}

static inline void ui_console_frame(const fb_ctx_t *fb,
                                    const touch_ctx_t *touch,
                                    const int *raw_x,
                                    const int *raw_y,
                                    int points,
                                    ui_touch_latch_t *cfg_latch,
                                    const ui_console_model_t *model,
                                    ui_console_io_t *io) {
    if (io) {
        memset(io, 0, sizeof(*io));
    }

    if (!fb || !fb->map || !model || !model->voice_enabled || !model->log) {
        if (cfg_latch) {
            (void)ui_im_rising_edge(0, cfg_latch);
        }
        return;
    }

    ui_console_theme_t theme;
    ui_console_theme_default(fb, &theme);

    ui_layout_t layout;
    ui_layout_compute_default(fb, &layout);

    ui_console_interact(fb, touch, raw_x, raw_y, points, cfg_latch, model, io);

    ui_fill_rect(fb, 0, 0, fb->width, fb->height, theme.bg);
    ui_draw_text(fb,
                 (int)layout.margin,
                 (int)(layout.margin / 2),
                 "TOUCH VOICES + MIDI LOG",
                 theme.title,
                 2);

    static const char *labels[2] = {"VOICE 1", "VOICE 2"};
    for (int idx = 0; idx < 2; ++idx) {
        ui_rect_t vr = ui_layout_voice_button_rect(&layout, idx);
        ui_draw_button(fb,
                       (uint32_t)vr.x,
                       (uint32_t)vr.y,
                       (uint32_t)vr.w,
                       (uint32_t)vr.h,
                       labels[idx],
                       &theme.voice_theme[idx],
                       model->voice_enabled[idx],
                       2);
    }

    ui_rect_t cfg_rect = ui_layout_config_rect(&layout);
    char cfg_label[24];
    snprintf(cfg_label, sizeof(cfg_label), "CFG %s", model->config_enabled ? "ON" : "OFF");
    ui_draw_button(fb,
                   (uint32_t)cfg_rect.x,
                   (uint32_t)cfg_rect.y,
                   (uint32_t)cfg_rect.w,
                   (uint32_t)cfg_rect.h,
                   cfg_label,
                   &theme.cfg_theme,
                   model->config_enabled,
                   1);

    ui_console_draw_log_panel(fb, &layout, &theme, model);
}

#endif
