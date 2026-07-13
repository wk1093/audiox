#include "framebuffer/context.hpp"

#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <cstdint>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font8x8_basic.h"

static inline uint32_t packColor(const FramebufferContext *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (fb->bpp == 16) {
        uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        return c;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

void FramebufferContext::drawPixel(int x, int y, unsigned char r, unsigned char g, unsigned char b) {
    if (!isReady()) {
        return;
    }

    if (!stagingBuffer) {
        return;
    }

    if (x < 0 || x >= (int)width || y < 0 || y >= (int)height) {
        return;
    }

    uint32_t color = packColor(this, r, g, b);

    uint8_t *row = stagingBuffer + y * line_length;
    if (bpp == 16) {
        ((uint16_t *)row)[x] = (uint16_t)color;
    } else if (bpp == 24) {
        uint8_t *px = row + x * 3;
        px[0] = (uint8_t)(color & 0xFF);
        px[1] = (uint8_t)((color >> 8) & 0xFF);
        px[2] = (uint8_t)((color >> 16) & 0xFF);
    } else if (bpp == 32) {
        ((uint32_t *)row)[x] = color;
    }
}

void FramebufferContext::drawRect(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b) {
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;

    if (x_end > width) x_end = width;
    if (y_end > height) y_end = height;

    // TODO: optimize this
    for (uint32_t yy = y; yy < y_end; ++yy) {
        for (uint32_t xx = x; xx < x_end; ++xx) {
            drawPixel(xx, yy, r, g, b);
        }
    }
}

void FramebufferContext::drawImage(int x, int y, int w, int h, const unsigned char *data) {
    // we assume PPM format for now
    // read header
    int img_w = 0, img_h = 0, img_maxval = 0;
    if (sscanf((const char *)data, "P6\n%d %d\n%d\n", &img_w, &img_h, &img_maxval) != 3) {
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

    for (int yy = 0; yy < h; ++yy) {
        for (int xx = 0; xx < w; ++xx) {
            // Calculate source coordinates with nearest neighbor sampling
            int src_x = (xx * img_w) / w;
            int src_y = (yy * img_h) / h;
            
            // Clamp to image bounds
            if (src_x >= img_w) src_x = img_w - 1;
            if (src_y >= img_h) src_y = img_h - 1;
            
            int src_idx = (src_y * img_w + src_x) * 3;
            unsigned char r = pixel_data[src_idx];
            unsigned char g = pixel_data[src_idx + 1];
            unsigned char b = pixel_data[src_idx + 2];
            drawPixel(x + xx, y + yy, r, g, b);
        }
    }

}

void FramebufferContext::drawText(int x, int y, const char *text, unsigned char r, unsigned char g, unsigned char b, int size) {
    if (size <= 0) {
        size = 1;
    }

    for (const char *p = text; *p; ++p) {
        unsigned char c = (unsigned char)(*p);
        if (c >= 128) {
            continue;
        }

        for (int row = 0; row < 8; ++row) {
            unsigned char row_data = font8x8_basic[c][row];
            for (int col = 0; col < 8; ++col) {
                if (row_data & (1 << col)) {
                    for (int sy = 0; sy < size; ++sy) {
                        for (int sx = 0; sx < size; ++sx) {
                            drawPixel(x + col * size + sx, y + row * size + sy, r, g, b);
                        }
                    }
                }
            }
        }

        x += 8 * size;
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
        return;
    }

    vinfo.xoffset = 0;
    vinfo.yoffset = draw_page ? height : 0;
    vinfo.activate = FB_ACTIVATE_VBL;
    if (ioctl(fd, FBIOPAN_DISPLAY, &vinfo) < 0) {
        vinfo.activate = FB_ACTIVATE_NOW;
        (void)ioctl(fd, FBIOPAN_DISPLAY, &vinfo);
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
        draw_page = 0;
        draw_offset = 0;
    }

    if (stagingBuffer) {
        memset(stagingBuffer, 0, page_len);
    }
}