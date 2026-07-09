#ifndef UI_PPM_H
#define UI_PPM_H

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdint.h>
#include <string.h>

#include "ui_fb.h"

typedef struct ui_ppm_image {
    uint8_t *rgb;
    uint32_t width;
    uint32_t height;
    int loaded;
    int failed;
} ui_ppm_image_t;

static inline int ui_ppm_next_token(FILE *fp, char *token, size_t token_cap) {
    if (!fp || !token || token_cap == 0) {
        return 0;
    }

    int c = fgetc(fp);
    for (;;) {
        while (c != EOF && isspace(c)) {
            c = fgetc(fp);
        }
        if (c != '#') {
            break;
        }
        while (c != EOF && c != '\n') {
            c = fgetc(fp);
        }
        c = fgetc(fp);
    }

    if (c == EOF) {
        return 0;
    }

    size_t idx = 0;
    while (c != EOF && !isspace(c) && c != '#') {
        if (idx + 1 < token_cap) {
            token[idx++] = (char)c;
        }
        c = fgetc(fp);
    }
    token[idx] = '\0';

    if (c == '#') {
        while (c != EOF && c != '\n') {
            c = fgetc(fp);
        }
    }

    return idx > 0;
}

static inline int ui_ppm_load(ui_ppm_image_t *img, const char *path) {
    if (!img || !path) {
        return -1;
    }

    if (img->loaded) {
        return 0;
    }
    if (img->failed) {
        return -1;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        img->failed = 1;
        return -1;
    }

    char token[64];
    if (!ui_ppm_next_token(fp, token, sizeof(token)) || strcmp(token, "P6") != 0) {
        fclose(fp);
        img->failed = 1;
        return -1;
    }

    if (!ui_ppm_next_token(fp, token, sizeof(token))) {
        fclose(fp);
        img->failed = 1;
        return -1;
    }
    unsigned long w = strtoul(token, NULL, 10);

    if (!ui_ppm_next_token(fp, token, sizeof(token))) {
        fclose(fp);
        img->failed = 1;
        return -1;
    }
    unsigned long h = strtoul(token, NULL, 10);

    if (!ui_ppm_next_token(fp, token, sizeof(token))) {
        fclose(fp);
        img->failed = 1;
        return -1;
    }
    unsigned long maxval = strtoul(token, NULL, 10);

    if (w == 0 || h == 0 || w > 4096 || h > 4096 || maxval != 255) {
        fclose(fp);
        img->failed = 1;
        return -1;
    }

    size_t px_count = (size_t)w * (size_t)h;
    if (px_count > (SIZE_MAX / 3u)) {
        fclose(fp);
        img->failed = 1;
        return -1;
    }

    size_t rgb_len = px_count * 3u;
    uint8_t *rgb = (uint8_t *)malloc(rgb_len);
    if (!rgb) {
        fclose(fp);
        img->failed = 1;
        return -1;
    }

    size_t nread = fread(rgb, 1, rgb_len, fp);
    fclose(fp);
    if (nread != rgb_len) {
        free(rgb);
        img->failed = 1;
        return -1;
    }

    img->rgb = rgb;
    img->width = (uint32_t)w;
    img->height = (uint32_t)h;
    img->loaded = 1;
    return 0;
}

static inline void ui_ppm_draw(const ui_ppm_image_t *img,
                               const fb_ctx_t *fb,
                               int x,
                               int y,
                               int max_w,
                               int max_h) {
    if (!img || !fb || !fb->map || !img->loaded || !img->rgb || max_w <= 0 || max_h <= 0) {
        return;
    }

    int src_w = (int)img->width;
    int src_h = (int)img->height;
    if (src_w <= 0 || src_h <= 0) {
        return;
    }

    int dst_w = src_w;
    int dst_h = src_h;

    if (dst_w > max_w) {
        dst_h = (dst_h * max_w) / dst_w;
        dst_w = max_w;
    }
    if (dst_h > max_h) {
        dst_w = (dst_w * max_h) / dst_h;
        dst_h = max_h;
    }
    if (dst_w <= 0 || dst_h <= 0) {
        return;
    }

    int dx0 = x + (max_w - dst_w) / 2;
    int dy0 = y + (max_h - dst_h) / 2;

    for (int dy = 0; dy < dst_h; ++dy) {
        int sy = (dy * src_h) / dst_h;
        for (int dx = 0; dx < dst_w; ++dx) {
            int sx = (dx * src_w) / dst_w;
            size_t idx = ((size_t)sy * (size_t)src_w + (size_t)sx) * 3u;
            uint8_t r = img->rgb[idx + 0];
            uint8_t g = img->rgb[idx + 1];
            uint8_t b = img->rgb[idx + 2];
            fb_put_pixel(fb,
                         (uint32_t)(dx0 + dx),
                         (uint32_t)(dy0 + dy),
                         fb_pack_color(fb, r, g, b));
        }
    }
}

static inline void ui_ppm_free(ui_ppm_image_t *img) {
    if (img && img->rgb) {
        free(img->rgb);
        img->rgb = NULL;
        img->loaded = 0;
        img->failed = 0;
    }
}

#endif
