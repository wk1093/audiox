#ifndef UI_CONSOLE_H
#define UI_CONSOLE_H

#include <string.h>

#include "audio/types.h"
#include "ui_immediate.h"

#define UI_TAB_SOUNDBOARD 0
#define UI_TAB_CONFIG 1
#define UI_TAB_LOG 2
#define UI_TAB_CONTROL 3
#define UI_TAB_COUNT 4

#define UI_TAP_MOVE_TOLERANCE_PX 18
#define UI_TAB_SWIPE_MIN_DX_PX 64
#define UI_TAB_SWIPE_MAX_DY_PX 36

#define UI_LOG_FILTER_INFO (1u << UI_LOG_INFO)
#define UI_LOG_FILTER_WARN (1u << UI_LOG_WARN)
#define UI_LOG_FILTER_ERROR (1u << UI_LOG_ERROR)
#define UI_LOG_FILTER_ALL (UI_LOG_FILTER_INFO | UI_LOG_FILTER_WARN | UI_LOG_FILTER_ERROR)

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
    uint32_t log_info;
    uint32_t log_warn;
    uint32_t log_error;
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
    uint8_t log_filter_mask;
    const ui_log_t *log;
} ui_console_model_t;

typedef struct ui_console_io {
    int config_toggled;
    int selected_tab;
    int tab_step;
    int log_filter_toggled[3];
    int control_sync;
    int control_restart;
    int control_shutdown;
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
    theme->log_info = fb_pack_color(fb, 182, 196, 212);
    theme->log_warn = fb_pack_color(fb, 236, 184, 96);
    theme->log_error = fb_pack_color(fb, 245, 110, 110);
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

static inline ui_rect_t ui_console_log_filter_button_rect(const ui_rect_t *content, int idx) {
    int h = ui_text_height(1) + 10;
    int w = 72;
    int gap = 6;
    int x = content->x + 8 + idx * (w + gap);
    int y = content->y + 8;
    return ui_rect_make(x, y, w, h);
}

static inline ui_rect_t ui_console_control_button_rect(const ui_rect_t *content, int idx) {
    int gap = 8;
    int x = content->x + 12;
    int w = content->w - 24;
    if (w < 80) {
        w = 80;
    }

    int h = (content->h - 40 - (2 * gap)) / 3;
    if (h < 28) {
        h = 28;
    }

    int y = content->y + 20 + idx * (h + gap);
    return ui_rect_make(x, y, w, h);
}

static inline void ui_console_draw_tab_bar(const fb_ctx_t *fb,
                                           const ui_console_theme_t *theme,
                                           int active_tab) {
    static const char *labels[UI_TAB_COUNT] = {
        "SOUNDBOARD",
        "CONFIG",
        "LOG",
        "CONTROL"
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

    static const char *filter_labels[3] = {"INFO", "WARN", "ERROR"};
    for (int i = 0; i < 3; ++i) {
        uint8_t mask = (uint8_t)(1u << i);
        ui_rect_t b = ui_console_log_filter_button_rect(content, i);
        ui_draw_button(fb,
                       (uint32_t)b.x,
                       (uint32_t)b.y,
                       (uint32_t)b.w,
                       (uint32_t)b.h,
                       filter_labels[i],
                       &theme->tab_theme,
                       (model->log_filter_mask & mask) ? 1 : 0,
                       1);
    }

    int text_x = content->x + 8;
    int line_h = ui_text_height(1) + 2;
    int first_line_y = content->y + ui_text_height(1) + 24;
    int max_lines = (content->h - (first_line_y - content->y) - 8) / line_h;
    if (max_lines < 1) {
        return;
    }

    int max_chars = (content->w - 16) / 8;
    if (max_chars < 8) {
        max_chars = 8;
    }

    int rows_used = 0;
    int start_idx = model->log->count;
    for (int i = model->log->count - 1; i >= 0; --i) {
        const char *line = NULL;
        uint8_t level = UI_LOG_INFO;
        if (ui_log_get_line_with_level(model->log, i, &line, &level) != 0 || !line) {
            continue;
        }

        if ((model->log_filter_mask & (uint8_t)(1u << level)) == 0) {
            continue;
        }

        size_t len = strlen(line);
        int wraps = (int)((len + (size_t)max_chars - 1) / (size_t)max_chars);
        if (wraps < 1) {
            wraps = 1;
        }

        if (rows_used + wraps > max_lines) {
            break;
        }

        rows_used += wraps;
        start_idx = i;
    }

    int row = 0;
    for (int i = start_idx; i < model->log->count && row < max_lines; ++i) {
        const char *line = NULL;
        uint8_t level = UI_LOG_INFO;
        if (ui_log_get_line_with_level(model->log, i, &line, &level) != 0 || !line) {
            continue;
        }

        if ((model->log_filter_mask & (uint8_t)(1u << level)) == 0) {
            continue;
        }

        uint32_t color = theme->log_info;
        if (level == UI_LOG_WARN) {
            color = theme->log_warn;
        } else if (level == UI_LOG_ERROR) {
            color = theme->log_error;
        }

        size_t len = strlen(line);
        size_t pos = 0;
        while (row < max_lines) {
            char chunk[97];
            if (pos >= len) {
                if (pos == 0) {
                    chunk[0] = '\0';
                } else {
                    break;
                }
            }

            size_t remaining = (pos < len) ? (len - pos) : 0;
            size_t take = remaining;
            if (take > (size_t)max_chars) {
                take = (size_t)max_chars;
            }
            if (take > sizeof(chunk) - 1) {
                take = sizeof(chunk) - 1;
            }

            if (take > 0) {
                memcpy(chunk, line + pos, take);
            }
            chunk[take] = '\0';

            ui_draw_text(fb, text_x, first_line_y + (row * line_h), chunk, color, 1);
            ++row;

            if (pos >= len) {
                break;
            }
            pos += take;
            if (take == 0) {
                break;
            }
            if (pos >= len) {
                break;
            }
        }
    }
}

static inline void ui_console_draw_control_panel(const fb_ctx_t *fb,
                                                 const ui_rect_t *content,
                                                 const ui_console_theme_t *theme,
                                                 const ui_console_model_t *model) {
    (void)model;

    ui_console_draw_panel_bg(fb, content, theme);
    ui_draw_text(fb, content->x + 8, content->y + 8, "CONTROL", theme->header, 1);

    static const char *labels[3] = {
        "SYNC",
        "RESTART",
        "SHUTDOWN"
    };

    for (int i = 0; i < 3; ++i) {
        ui_rect_t b = ui_console_control_button_rect(content, i);
        ui_draw_button(fb,
                       (uint32_t)b.x,
                       (uint32_t)b.y,
                       (uint32_t)b.w,
                       (uint32_t)b.h,
                       labels[i],
                       &theme->tab_theme,
                       0,
                       2);
    }
}

static inline void ui_console_interact(const fb_ctx_t *fb,
                                       const touch_ctx_t *touch,
                                       const int *raw_x,
                                       const int *raw_y,
                                       int points,
                                       ui_touch_latch_t *cfg_latch,
                                       ui_touch_latch_t tab_latches[UI_TAB_COUNT],
                                       ui_touch_latch_t log_filter_latches[3],
                                       ui_touch_latch_t control_latches[3],
                                       ui_touch_latch_t *swipe_latch,
                                       const ui_console_model_t *model,
                                       ui_console_io_t *io) {
    if (io) {
        memset(io, 0, sizeof(*io));
        io->selected_tab = -1;
        io->tab_step = 0;
    }

    if (!fb || !fb->map || !model) {
        if (cfg_latch) {
            (void)ui_im_rising_edge(0, cfg_latch);
        }
        return;
    }

    ui_im_ctx_t ui;
    ui_im_begin(&ui, fb, touch, raw_x, raw_y, points);

    if (io && swipe_latch) {
        io->tab_step = ui_im_release_swipe_horizontal(&ui,
                                                      swipe_latch,
                                                      UI_TAB_SWIPE_MIN_DX_PX,
                                                      UI_TAB_SWIPE_MAX_DY_PX);
    }

    for (int i = 0; i < UI_TAB_COUNT; ++i) {
        ui_rect_t tab = ui_console_tab_rect(fb, i);
        if (io && tab_latches &&
            ui_im_release_tap_rect(&ui, &tab, &tab_latches[i], UI_TAP_MOVE_TOLERANCE_PX)) {
            io->selected_tab = i;
        }
    }

    ui_rect_t content = ui_console_content_rect(fb);
    ui_rect_t cfg_rect = ui_console_config_button_rect(&content);
    if (io) {
        io->config_toggled = (model->active_tab == UI_TAB_CONFIG)
                                 ? ui_im_release_tap_rect(&ui, &cfg_rect, cfg_latch, UI_TAP_MOVE_TOLERANCE_PX)
                                 : 0;

        if (model->active_tab == UI_TAB_LOG && log_filter_latches) {
            for (int i = 0; i < 3; ++i) {
                ui_rect_t b = ui_console_log_filter_button_rect(&content, i);
                io->log_filter_toggled[i] = ui_im_release_tap_rect(&ui,
                                                                    &b,
                                                                    &log_filter_latches[i],
                                                                    UI_TAP_MOVE_TOLERANCE_PX);
            }
        }

        if (model->active_tab == UI_TAB_CONTROL && control_latches) {
            ui_rect_t sync_btn = ui_console_control_button_rect(&content, 0);
            ui_rect_t restart_btn = ui_console_control_button_rect(&content, 1);
            ui_rect_t shutdown_btn = ui_console_control_button_rect(&content, 2);

            io->control_sync = ui_im_release_tap_rect(&ui,
                                                      &sync_btn,
                                                      &control_latches[0],
                                                      UI_TAP_MOVE_TOLERANCE_PX);
            io->control_restart = ui_im_release_tap_rect(&ui,
                                                         &restart_btn,
                                                         &control_latches[1],
                                                         UI_TAP_MOVE_TOLERANCE_PX);
            io->control_shutdown = ui_im_release_tap_rect(&ui,
                                                          &shutdown_btn,
                                                          &control_latches[2],
                                                          UI_TAP_MOVE_TOLERANCE_PX);
        }
    }
}

static inline void ui_console_frame(const fb_ctx_t *fb,
                                    const touch_ctx_t *touch,
                                    const int *raw_x,
                                    const int *raw_y,
                                    int points,
                                    ui_touch_latch_t *cfg_latch,
                                    ui_touch_latch_t tab_latches[UI_TAB_COUNT],
                                    ui_touch_latch_t log_filter_latches[3],
                                    ui_touch_latch_t control_latches[3],
                                    ui_touch_latch_t *swipe_latch,
                                    const ui_console_model_t *model,
                                    ui_console_io_t *io) {
    if (!fb || !fb->map || !model || !model->log) {
        if (cfg_latch) {
            (void)ui_im_rising_edge(0, cfg_latch);
        }
        return;
    }

    (void)touch;
    (void)raw_x;
    (void)raw_y;
    (void)points;
    (void)tab_latches;
    (void)log_filter_latches;
    (void)control_latches;
    (void)swipe_latch;
    (void)io;

    ui_console_theme_t theme;
    ui_console_theme_default(fb, &theme);

    ui_fill_rect(fb, 0, 0, fb->width, fb->height, theme.bg);
    ui_console_draw_tab_bar(fb, &theme, model->active_tab);

    ui_rect_t content = ui_console_content_rect(fb);

    if (model->active_tab == UI_TAB_CONFIG) {
        ui_console_draw_config_panel(fb, &content, &theme, model);
    } else if (model->active_tab == UI_TAB_LOG) {
        ui_console_draw_log_panel(fb, &content, &theme, model);
    } else if (model->active_tab == UI_TAB_CONTROL) {
        ui_console_draw_control_panel(fb, &content, &theme, model);
    } else {
        ui_console_draw_soundboard_panel(fb, &content, &theme, model);
    }
}

static inline void ui_console_draw_tab_bar_only(const fb_ctx_t *fb,
                                                const ui_console_model_t *model) {
    if (!fb || !fb->map || !model) {
        return;
    }

    ui_console_theme_t theme;
    ui_console_theme_default(fb, &theme);

    int margin = (int)(fb->width / 24);
    if (margin < 8) {
        margin = 8;
    }

    int y = margin / 2;
    int h = ui_text_height(1) + 12;
    int w = (int)fb->width - (margin * 2);
    if (w > 0 && h > 0) {
        ui_fill_rect(fb,
                     (uint32_t)margin,
                     (uint32_t)y,
                     (uint32_t)w,
                     (uint32_t)h,
                     theme.bg);
    }

    ui_console_draw_tab_bar(fb, &theme, model->active_tab);
}

static inline void ui_console_draw_active_panel_only(const fb_ctx_t *fb,
                                                     const ui_console_model_t *model) {
    if (!fb || !fb->map || !model || !model->log) {
        return;
    }

    ui_console_theme_t theme;
    ui_console_theme_default(fb, &theme);
    ui_rect_t content = ui_console_content_rect(fb);

    if (model->active_tab == UI_TAB_CONFIG) {
        ui_console_draw_config_panel(fb, &content, &theme, model);
    } else if (model->active_tab == UI_TAB_LOG) {
        ui_console_draw_log_panel(fb, &content, &theme, model);
    } else if (model->active_tab == UI_TAB_CONTROL) {
        ui_console_draw_control_panel(fb, &content, &theme, model);
    } else {
        ui_console_draw_soundboard_panel(fb, &content, &theme, model);
    }
}

#endif
