#ifndef UI_FB_H
#define UI_FB_H

#include <stdint.h>
#include <string.h>

#include "font8x8_basic.h"
#include "fb_touch.h"

typedef struct ui_button_theme {
    uint32_t fill;
    uint32_t fill_active;
    uint32_t border;
    uint32_t border_active;
    uint32_t text;
    uint32_t text_active;
} ui_button_theme_t;

static inline const uint8_t *ui_font_lookup_glyph(char ch) {
    unsigned char c = (unsigned char)ch;
    if (c < 128) {
        return font8x8_basic[c];
    }
    return font8x8_basic['?'];
}

static inline void ui_fill_rect(const fb_ctx_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    fb_fill_rect(fb, x, y, w, h, color);
}

static inline void ui_stroke_rect(const fb_ctx_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color, uint32_t thickness) {
    if (!fb || !fb->map || w == 0 || h == 0 || thickness == 0) {
        return;
    }

    if (thickness * 2 >= w || thickness * 2 >= h) {
        fb_fill_rect(fb, x, y, w, h, color);
        return;
    }

    fb_fill_rect(fb, x, y, w, thickness, color);
    fb_fill_rect(fb, x, y + h - thickness, w, thickness, color);
    fb_fill_rect(fb, x, y + thickness, thickness, h - (thickness * 2), color);
    fb_fill_rect(fb, x + w - thickness, y + thickness, thickness, h - (thickness * 2), color);
}

static inline void ui_draw_char(const fb_ctx_t *fb, int x, int y, char ch, uint32_t color, uint32_t scale) {
    if (!fb || !fb->map) {
        return;
    }

    if (scale == 0) {
        scale = 1;
    }

    const uint8_t *glyph = ui_font_lookup_glyph(ch);
    for (int row = 0; row < 8; ++row) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; ++col) {
            if ((bits & (uint8_t)(1u << col)) == 0) {
                continue;
            }

            int px = x + (col * (int)scale);
            int py = y + (row * (int)scale);
            fb_fill_rect(fb, (uint32_t)px, (uint32_t)py, scale, scale, color);
        }
    }
}

static inline int ui_text_width(const char *text, uint32_t scale) {
    if (!text) {
        return 0;
    }
    if (scale == 0) {
        scale = 1;
    }

    int width = 0;
    for (const char *p = text; *p != '\0'; ++p) {
        width += (int)(8 * scale);
    }
    return width;
}

static inline int ui_text_height(uint32_t scale) {
    if (scale == 0) {
        scale = 1;
    }
    return (int)(8 * scale);
}

static inline void ui_draw_text(const fb_ctx_t *fb, int x, int y, const char *text, uint32_t color, uint32_t scale) {
    if (!text) {
        return;
    }

    int cursor_x = x;
    for (const char *p = text; *p != '\0'; ++p) {
        ui_draw_char(fb, cursor_x, y, *p, color, scale);
        cursor_x += (int)(8 * (scale == 0 ? 1 : scale));
    }
}

static inline void ui_draw_button(const fb_ctx_t *fb,
                                  uint32_t x,
                                  uint32_t y,
                                  uint32_t w,
                                  uint32_t h,
                                  const char *label,
                                  const ui_button_theme_t *theme,
                                  int active,
                                  uint32_t text_scale) {
    if (!fb || !fb->map || !theme || w == 0 || h == 0) {
        return;
    }

    uint32_t fill = active ? theme->fill_active : theme->fill;
    uint32_t border = active ? theme->border_active : theme->border;
    uint32_t text = active ? theme->text_active : theme->text;

    ui_fill_rect(fb, x, y, w, h, border);

    if (w > 8 && h > 8) {
        ui_fill_rect(fb, x + 4, y + 4, w - 8, h - 8, fill);
    }

    if (!label || label[0] == '\0') {
        return;
    }

    if (text_scale == 0) {
        text_scale = 1;
    }

    int tw = ui_text_width(label, text_scale);
    int th = ui_text_height(text_scale);
    int tx = (int)x + ((int)w - tw) / 2;
    int ty = (int)y + ((int)h - th) / 2;

    if (tx < (int)x + 6) {
        tx = (int)x + 6;
    }
    if (ty < (int)y + 6) {
        ty = (int)y + 6;
    }

    ui_draw_text(fb, tx, ty, label, text, text_scale);
}

#endif
