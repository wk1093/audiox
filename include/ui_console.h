#ifndef UI_CONSOLE_H
#define UI_CONSOLE_H

#include <string.h>

#include "audio/types.h"
#include "ui_immediate.h"

#define UI_TAB_SOUNDBOARD 0
#define UI_TAB_CONFIG 1
#define UI_TAB_LOG 2
#define UI_TAB_COUNT 3

typedef struct ui_console_theme {
    ui_button_theme_t tab_theme;
    ui_button_theme_t cfg_theme;
    uint32_t bg;
    uint32_t title;
    uint32_t panel_border;
    uint32_t panel_fill;
    uint32_t header;
    uint32_t text;
    uint32_t status_ok;
    uint32_t status_wait;
    uint32_t status_active;
} ui_console_theme_t;

typedef struct ui_console_model {
    const int *voice_enabled;
    const char (*voice_name)[AUDIO_MAX_VOICE_NAME];
    int voice_count;
    uint8_t voice_note_base;
    int midi_connected;
    const char *midi_dev;
    int config_enabled;
    int config_mount_active;
    int active_tab;
    const ui_log_t *log;
} ui_console_model_t;

typedef struct ui_console_io {
    int config_toggled;
    int selected_tab;
} ui_console_io_t;

static inline void ui_console_theme_default(const fb_ctx_t *fb, ui_console_theme_t *theme) {
    memset(theme, 0, sizeof(*theme));

    theme->bg = fb_pack_color(fb, 10, 13, 18);
    theme->title = fb_pack_color(fb, 222, 230, 240);

    theme->tab_theme.fill = fb_pack_color(fb, 30, 35, 42);
    theme->tab_theme.fill_active = fb_pack_color(fb, 28, 106, 184);
    theme->tab_theme.border = fb_pack_color(fb, 100, 116, 132);
    theme->tab_theme.border_active = fb_pack_color(fb, 178, 220, 255);
    theme->tab_theme.text = fb_pack_color(fb, 208, 216, 226);
    theme->tab_theme.text_active = fb_pack_color(fb, 248, 252, 255);

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
    theme->status_active = fb_pack_color(fb, 112, 176, 255);
}

static inline ui_rect_t ui_console_tab_rect(const fb_ctx_t *fb, int idx) {
    int margin = (int)(fb->width / 24);
    if (margin < 8) {
        margin = 8;
    }

    int gap = margin / 2;
    if (gap < 6) {
        gap = 6;
    }

    int y = margin / 2;
    int h = ui_text_height(1) + 12;
    int total_gap = gap * (UI_TAB_COUNT - 1);
    int w = ((int)fb->width - (margin * 2) - total_gap) / UI_TAB_COUNT;
    if (w < 48) {
        w = 48;
    }

    int x = margin + idx * (w + gap);
    return ui_rect_make(x, y, w, h);
}

static inline ui_rect_t ui_console_content_rect(const fb_ctx_t *fb) {
    int margin = (int)(fb->width / 24);
    if (margin < 8) {
        margin = 8;
    }

    int gap = margin / 2;
    if (gap < 6) {
        gap = 6;
    }

    ui_rect_t tab0 = ui_console_tab_rect(fb, 0);
    int x = margin;
    int y = tab0.y + tab0.h + gap;
    int w = (int)fb->width - (margin * 2);
    int h = (int)fb->height - y - margin;
    if (h < 16) {
        h = 16;
    }

    return ui_rect_make(x, y, w, h);
}

static inline ui_rect_t ui_console_config_button_rect(const ui_rect_t *content) {
    int w = 112;
    int h = ui_text_height(1) + 12;
    int x = content->x + content->w - w - 8;
    int y = content->y + 8;

    if (x < content->x + 8) {
        x = content->x + 8;
    }

    return ui_rect_make(x, y, w, h);
}

static inline void ui_console_draw_tab_bar(const fb_ctx_t *fb,
                                           const ui_console_theme_t *theme,
                                           int active_tab) {
    static const char *labels[UI_TAB_COUNT] = {
        "SOUNDBOARD",
        "CONFIG",
        "LOG"
    };

    for (int i = 0; i < UI_TAB_COUNT; ++i) {
        ui_rect_t tab = ui_console_tab_rect(fb, i);
        ui_draw_button(fb,
                       (uint32_t)tab.x,
                       (uint32_t)tab.y,
                       (uint32_t)tab.w,
                       (uint32_t)tab.h,
                       labels[i],
                       &theme->tab_theme,
                       active_tab == i,
                       1);
    }
}

static inline void ui_console_draw_panel_bg(const fb_ctx_t *fb,
                                            const ui_rect_t *content,
                                            const ui_console_theme_t *theme) {
    ui_fill_rect(fb,
                 (uint32_t)content->x,
                 (uint32_t)content->y,
                 (uint32_t)content->w,
                 (uint32_t)content->h,
                 theme->panel_border);

    if (content->w > 4 && content->h > 4) {
        ui_fill_rect(fb,
                     (uint32_t)(content->x + 2),
                     (uint32_t)(content->y + 2),
                     (uint32_t)(content->w - 4),
                     (uint32_t)(content->h - 4),
                     theme->panel_fill);
    }
}

static inline void ui_console_draw_soundboard_panel(const fb_ctx_t *fb,
                                                    const ui_rect_t *content,
                                                    const ui_console_theme_t *theme,
                                                    const ui_console_model_t *model) {
    ui_console_draw_panel_bg(fb, content, theme);

    char line[96];
    snprintf(line,
             sizeof(line),
             "SOUNDBOARD SLOTS=%d NOTE_BASE=%u",
             model->voice_count,
             (unsigned)model->voice_note_base);
    ui_draw_text(fb, content->x + 8, content->y + 8, line, theme->header, 1);

    int line_h = ui_text_height(1) + 2;
    int max_rows = (content->h - 24) / line_h;
    if (max_rows < 1) {
        return;
    }

    int display = model->voice_count;
    if (display > max_rows) {
        display = max_rows;
    }

    for (int i = 0; i < display; ++i) {
        const char *name = "(unnamed)";
        if (model->voice_name && model->voice_name[i][0]) {
            name = model->voice_name[i];
        }

        int note = (int)model->voice_note_base + i;
        snprintf(line,
                 sizeof(line),
                 "N%03d %-24s %s",
                 note,
                 name,
                 (model->voice_enabled && model->voice_enabled[i]) ? "PLAY" : "IDLE");
        ui_draw_text(fb,
                     content->x + 8,
                     content->y + 8 + ui_text_height(1) + 4 + (i * line_h),
                     line,
                     (model->voice_enabled && model->voice_enabled[i]) ? theme->status_active : theme->text,
                     1);
    }
}

static inline void ui_console_draw_config_panel(const fb_ctx_t *fb,
                                                const ui_rect_t *content,
                                                const ui_console_theme_t *theme,
                                                const ui_console_model_t *model) {
    ui_console_draw_panel_bg(fb, content, theme);

    ui_draw_text(fb, content->x + 8, content->y + 8, "CONFIGURATION", theme->header, 1);
    ui_draw_text(fb,
                 content->x + 8,
                 content->y + 8 + ui_text_height(1) + 4,
                 model->config_mount_active ? "PERSISTENCE: MOUNTED" : "PERSISTENCE: UNMOUNTED",
                 model->config_mount_active ? theme->status_ok : theme->status_wait,
                 1);

    ui_rect_t cfg_rect = ui_console_config_button_rect(content);
    char cfg_label[24];
    snprintf(cfg_label, sizeof(cfg_label), "CFG %s", model->config_enabled ? "ON" : "OFF");
    ui_draw_button(fb,
                   (uint32_t)cfg_rect.x,
                   (uint32_t)cfg_rect.y,
                   (uint32_t)cfg_rect.w,
                   (uint32_t)cfg_rect.h,
                   cfg_label,
                   &theme->cfg_theme,
                   model->config_enabled,
                   1);
}

static inline void ui_console_draw_log_panel(const fb_ctx_t *fb,
                                             const ui_rect_t *content,
                                             const ui_console_theme_t *theme,
                                             const ui_console_model_t *model) {
    if (!model->log) {
        return;
    }

    ui_console_draw_panel_bg(fb, content, theme);

    int text_x = content->x + 8;
    int text_y = content->y + 8;
    ui_draw_text(fb, text_x, text_y, "MIDI + CONFIG LOG", theme->header, 1);

    char status_line[160];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-truncation"
    snprintf(status_line,
             sizeof(status_line),
             "MIDI: %s%s%s",
             model->midi_connected ? "CONNECTED" : "WAITING",
             (model->midi_connected && model->midi_dev && model->midi_dev[0] != '\0') ? " " : "",
             (model->midi_connected && model->midi_dev && model->midi_dev[0] != '\0') ? model->midi_dev : "");
#pragma GCC diagnostic pop
    ui_draw_text(fb,
                 text_x,
                 text_y + ui_text_height(1) + 2,
                 status_line,
                 model->midi_connected ? theme->status_ok : theme->status_wait,
                 1);

    ui_draw_text(fb,
                 text_x,
                 text_y + ((ui_text_height(1) + 2) * 2),
                 model->config_mount_active ? "CFG FS: MOUNTED" : "CFG FS: UNMOUNTED",
                 model->config_mount_active ? theme->status_ok : theme->status_wait,
                 1);

    int line_h = ui_text_height(1) + 2;
    int first_line_y = text_y + ((ui_text_height(1) + 2) * 3) + 2;
    int max_lines = (content->h - (first_line_y - content->y) - 8) / line_h;
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
        io->selected_tab = -1;
    }

    if (!fb || !fb->map || !model) {
        if (cfg_latch) {
            (void)ui_im_rising_edge(0, cfg_latch);
        }
        return;
    }

    ui_im_ctx_t ui;
    ui_im_begin(&ui, fb, touch, raw_x, raw_y, points);

    for (int i = 0; i < UI_TAB_COUNT; ++i) {
        ui_rect_t tab = ui_console_tab_rect(fb, i);
        if (ui_im_touching_rect(&ui, &tab) && io) {
            io->selected_tab = i;
        }
    }

    ui_rect_t content = ui_console_content_rect(fb);
    ui_rect_t cfg_rect = ui_console_config_button_rect(&content);
    int cfg_down = (model->active_tab == UI_TAB_CONFIG) ? ui_im_touching_rect(&ui, &cfg_rect) : 0;
    int toggled = ui_im_rising_edge(cfg_down, cfg_latch);
    if (io) {
        io->config_toggled = toggled;
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
        io->selected_tab = -1;
    }

    if (!fb || !fb->map || !model || !model->log) {
        if (cfg_latch) {
            (void)ui_im_rising_edge(0, cfg_latch);
        }
        return;
    }

    ui_console_theme_t theme;
    ui_console_theme_default(fb, &theme);

    ui_console_interact(fb, touch, raw_x, raw_y, points, cfg_latch, model, io);

    ui_fill_rect(fb, 0, 0, fb->width, fb->height, theme.bg);
    ui_console_draw_tab_bar(fb, &theme, model->active_tab);

    ui_rect_t content = ui_console_content_rect(fb);

    if (model->active_tab == UI_TAB_CONFIG) {
        ui_console_draw_config_panel(fb, &content, &theme, model);
    } else if (model->active_tab == UI_TAB_LOG) {
        ui_console_draw_log_panel(fb, &content, &theme, model);
    } else {
        ui_console_draw_soundboard_panel(fb, &content, &theme, model);
    }
}

#endif
