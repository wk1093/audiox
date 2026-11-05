#include "framebuffer/context.hpp"

#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <cstdint>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "font8x8_basic.h"

static inline uint32_t packColor(const FramebufferContext *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (fb->bpp == 16) {
        uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        return c;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline void storePixel(uint8_t *px, uint32_t bpp, uint32_t color) {
    if (bpp == 16) {
        memcpy(px, &color, 2);
    } else if (bpp == 24) {
        px[0] = (uint8_t)(color & 0xFF);
        px[1] = (uint8_t)((color >> 8) & 0xFF);
        px[2] = (uint8_t)((color >> 16) & 0xFF);
    } else if (bpp == 32) {
        memcpy(px, &color, 4);
    }
}

static inline bool drawable(const FramebufferContext *fb) {
    return fb->isReady() && fb->stagingBuffer &&
           (fb->bpp == 16 || fb->bpp == 24 || fb->bpp == 32);
}

// Clips [x, x + w) x [y, y + h) to the screen; returns false when nothing remains.
static inline bool clipRect(const FramebufferContext *fb, int &x, int &y, int &w, int &h) {
    if (w <= 0 || h <= 0) return false;
    long x0 = x, y0 = y, x1 = (long)x + w, y1 = (long)y + h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (long)fb->width) x1 = fb->width;
    if (y1 > (long)fb->height) y1 = fb->height;
    if (x0 >= x1 || y0 >= y1) return false;
    x = (int)x0;
    y = (int)y0;
    w = (int)(x1 - x0);
    h = (int)(y1 - y0);
    return true;
}

void FramebufferContext::drawPixel(int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (!drawable(this)) {
        return;
    }

    if (x < 0 || x >= (int)width || y < 0 || y >= (int)height) {
        return;
    }

    const uint32_t bytesPerPixel = bpp / 8;
    storePixel(stagingBuffer + (size_t)y * line_length + (size_t)x * bytesPerPixel, bpp,
               packColor(this, r, g, b));
}

void FramebufferContext::drawRect(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b) {
    if (!drawable(this) || !clipRect(this, x, y, w, h)) {
        return;
    }

    const uint32_t bytesPerPixel = bpp / 8;
    const uint32_t color = packColor(this, r, g, b);
    const size_t rowBytes = (size_t)w * bytesPerPixel;
    uint8_t *first = stagingBuffer + (size_t)y * line_length + (size_t)x * bytesPerPixel;
    if (bpp == 32) {
        uint32_t *px = (uint32_t *)first;
        for (int i = 0; i < w; ++i) px[i] = color;
    } else if (bpp == 16) {
        uint16_t *px = (uint16_t *)first;
        for (int i = 0; i < w; ++i) px[i] = (uint16_t)color;
    } else {
        for (int i = 0; i < w; ++i) storePixel(first + (size_t)i * 3, bpp, color);
    }
    uint8_t *row = first;
    for (int yy = 1; yy < h; ++yy) {
        row += line_length;
        memcpy(row, first, rowBytes);
    }
}

namespace {
// Pre-scaled, display-format copy of the last image drawn, so the logo redrawn every frame is a
// row memcpy instead of a header parse plus per-pixel nearest-neighbour scale.
struct ScaledImageCache {
    const unsigned char *source{nullptr};
    int w{0};
    int h{0};
    uint32_t bpp{0};
    uint8_t *pixels{nullptr};
    size_t capacity{0};
};
ScaledImageCache scaledImageCache;
}

void FramebufferContext::drawImage(int x, int y, int w, int h, const unsigned char *data) {
    if (!data || !drawable(this)) {
        return;
    }
    const uint32_t bytesPerPixel = bpp / 8;
    ScaledImageCache &cache = scaledImageCache;
    // w/h <= 0 means "native size", which needs the header to resolve.
    const bool hit = w > 0 && h > 0 && cache.pixels && cache.source == data &&
                     cache.bpp == bpp && cache.w == w && cache.h == h;
    if (!hit) {
        // we assume PPM format for now
        int img_w = 0, img_h = 0, img_maxval = 0;
        if (sscanf((const char *)data, "P6\n%d %d\n%d\n", &img_w, &img_h, &img_maxval) != 3 ||
            img_w <= 0 || img_h <= 0) {
            return;
        }

        // find start of pixel data
        const unsigned char *pixel_data = data;
        int newline_count = 0;
        while (newline_count < 3) {
            if (*pixel_data == '\n') {
                ++newline_count;
            }
            ++pixel_data;
        }

        // If w or h is 0, use original image dimensions
        if (w <= 0) w = img_w;
        if (h <= 0) h = img_h;

        if (!(cache.pixels && cache.source == data && cache.bpp == bpp && cache.w == w && cache.h == h)) {
            const size_t needed = (size_t)w * (size_t)h * bytesPerPixel;
            if (needed > cache.capacity) {
                uint8_t *grown = (uint8_t *)realloc(cache.pixels, needed);
                if (!grown) {
                    return;
                }
                cache.pixels = grown;
                cache.capacity = needed;
            }
            for (int yy = 0; yy < h; ++yy) {
                // Nearest neighbour sampling
                int src_y = (int)(((long)yy * img_h) / h);
                if (src_y >= img_h) src_y = img_h - 1;
                uint8_t *dst = cache.pixels + (size_t)yy * w * bytesPerPixel;
                for (int xx = 0; xx < w; ++xx) {
                    int src_x = (int)(((long)xx * img_w) / w);
                    if (src_x >= img_w) src_x = img_w - 1;
                    const unsigned char *src = pixel_data + ((size_t)src_y * img_w + src_x) * 3;
                    storePixel(dst + (size_t)xx * bytesPerPixel, bpp, packColor(this, src[0], src[1], src[2]));
                }
            }
            cache.source = data;
            cache.w = w;
            cache.h = h;
            cache.bpp = bpp;
        }
    }

    int cx = x, cy = y, cw = w, ch = h;
    if (!clipRect(this, cx, cy, cw, ch)) {
        return;
    }
    for (int yy = 0; yy < ch; ++yy) {
        const uint8_t *src = cache.pixels + ((size_t)(cy - y + yy) * w + (size_t)(cx - x)) * bytesPerPixel;
        memcpy(stagingBuffer + (size_t)(cy + yy) * line_length + (size_t)cx * bytesPerPixel, src,
               (size_t)cw * bytesPerPixel);
    }
}

void FramebufferContext::drawText(int x, int y, const char *text, unsigned char r, unsigned char g, unsigned char b, int size) {
    if (!text || !drawable(this)) {
        return;
    }
    if (size <= 0) {
        size = 1;
    }

    const int glyph = 8 * size;
    if (y >= (int)height || y + glyph <= 0) {
        return;
    }
    const uint32_t bytesPerPixel = bpp / 8;
    const uint32_t color = packColor(this, r, g, b);
    for (const char *p = text; *p; ++p) {
        if (x >= (int)width) {
            break;
        }
        unsigned char c = (unsigned char)(*p);
        if (c >= 128) {
            continue;
        }
        if (x + glyph <= 0) {
            x += glyph;
            continue;
        }

        for (int row = 0; row < 8; ++row) {
            unsigned char row_data = font8x8_basic[c][row];
            if (!row_data) continue;
            for (int col = 0; col < 8; ++col) {
                if (!(row_data & (1 << col))) continue;
                if (size == 1) {
                    const int px = x + col, py = y + row;
                    if (px < 0 || px >= (int)width || py < 0 || py >= (int)height) continue;
                    storePixel(stagingBuffer + (size_t)py * line_length + (size_t)px * bytesPerPixel, bpp, color);
                } else {
                    drawRect(x + col * size, y + row * size, size, size, r, g, b);
                }
            }
        }
        x += glyph;
    }
}

void FramebufferContext::present() {
    if (!isReady()) {
        return;
    }

    if (!stagingBuffer) {
        return;
    }

    uint8_t *target = buffer + draw_offset;
    memcpy(target, stagingBuffer, page_len);

    if (!pageflip_enabled) {
        return;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        printf("[FBUI] [WARN] Framebuffer query failed: %s\n", strerror(errno));
        release();
        return;
    }

    vinfo.xoffset = 0;
    vinfo.yoffset = draw_page ? height : 0;
    vinfo.activate = FB_ACTIVATE_VBL;
    if (ioctl(fd, FBIOPAN_DISPLAY, &vinfo) < 0) {
        vinfo.activate = FB_ACTIVATE_NOW;
        if (ioctl(fd, FBIOPAN_DISPLAY, &vinfo) < 0) {
            printf("[FBUI] [WARN] Framebuffer pan failed: %s\n", strerror(errno));
            if (errno == ENODEV || errno == ENXIO || errno == EIO) {
                release();
            } else {
                pageflip_enabled = 0;
                draw_offset = front_page ? page_len : 0;
                memcpy(buffer + draw_offset, stagingBuffer, page_len);
            }
            return;
        }
    }

    front_page = draw_page;
}

void FramebufferContext::beginFrame() {
    if (!isReady()) {
        return;
    }

    if (pageflip_enabled) {
        draw_page = (front_page ^ 1u) & 1u;
        draw_offset = draw_page ? page_len : 0;
    } else {
        draw_page = front_page;
        draw_offset = front_page ? page_len : 0;
    }

    if (stagingBuffer) {
        memset(stagingBuffer, 0, page_len);
    }
}