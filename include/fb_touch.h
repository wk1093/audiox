#ifndef FB_TOUCH_H
#define FB_TOUCH_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

typedef struct fb_ctx {
    int fd;
    uint8_t *map;
    size_t map_len;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t line_length;
    char dev_path[32];
} fb_ctx_t;

typedef struct touch_ctx {
    int fd;
    int last_x;
    int last_y;
} touch_ctx_t;

static inline uint32_t fb_pack_color(const fb_ctx_t *fb, uint8_t r, uint8_t g, uint8_t b) {
    if (fb->bpp == 16) {
        uint16_t c = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        return c;
    }
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline void fb_put_pixel(const fb_ctx_t *fb, uint32_t x, uint32_t y, uint32_t color) {
    if (!fb->map || x >= fb->width || y >= fb->height) {
        return;
    }

    uint8_t *row = fb->map + y * fb->line_length;
    if (fb->bpp == 16) {
        ((uint16_t *)row)[x] = (uint16_t)color;
    } else if (fb->bpp == 24) {
        uint8_t *px = row + x * 3;
        px[0] = (uint8_t)(color & 0xFF);
        px[1] = (uint8_t)((color >> 8) & 0xFF);
        px[2] = (uint8_t)((color >> 16) & 0xFF);
    } else if (fb->bpp == 32) {
        ((uint32_t *)row)[x] = color;
    }
}

static inline void fb_fill_rect(const fb_ctx_t *fb, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;

    if (x_end > fb->width) x_end = fb->width;
    if (y_end > fb->height) y_end = fb->height;

    for (uint32_t yy = y; yy < y_end; ++yy) {
        for (uint32_t xx = x; xx < x_end; ++xx) {
            fb_put_pixel(fb, xx, yy, color);
        }
    }
}

static inline int fb_open(fb_ctx_t *fb) {
    memset(fb, 0, sizeof(*fb));
    fb->fd = -1;

    for (int idx = 0; idx <= 3; ++idx) {
        char path[32];
        int path_len = snprintf(path, sizeof(path), "/dev/fb%d", idx);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            continue;
        }

        int fd = open(path, O_RDWR);
        if (fd < 0) {
            continue;
        }

        struct fb_fix_screeninfo finfo;
        struct fb_var_screeninfo vinfo;
        if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0 || ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
            close(fd);
            continue;
        }

        if (vinfo.xres == 0 || vinfo.yres == 0 || vinfo.bits_per_pixel == 0 || finfo.smem_len == 0) {
            close(fd);
            continue;
        }

        uint8_t *map = (uint8_t *)mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (map == MAP_FAILED) {
            close(fd);
            continue;
        }

        fb->fd = fd;
        fb->map = map;
        fb->width = vinfo.xres;
        fb->height = vinfo.yres;
        fb->bpp = vinfo.bits_per_pixel;
        fb->line_length = finfo.line_length;
        fb->map_len = finfo.smem_len;
        snprintf(fb->dev_path, sizeof(fb->dev_path), "%s", path);

        printf("[INIT] Framebuffer ready on %s: %ux%u @ %ubpp\n", fb->dev_path, fb->width, fb->height, fb->bpp);
        return 0;
    }

    printf("[INIT] [WARN] No usable framebuffer found in /dev/fb0..3.\n");
    printf("[INIT] [WARN] FKMS should expose /dev/fb0 after boot settles.\n");
    return -1;
}

static inline void fb_close(fb_ctx_t *fb) {
    if (fb->map) {
        munmap(fb->map, fb->map_len);
        fb->map = NULL;
    }

    if (fb->fd >= 0) {
        close(fb->fd);
    }
    fb->fd = -1;
}

static inline void fb_draw_status(const fb_ctx_t *fb, int enabled) {
    if (!fb->map) {
        return;
    }

    uint32_t bg = fb_pack_color(fb, 12, 14, 18);
    uint32_t panel = enabled ? fb_pack_color(fb, 26, 130, 78) : fb_pack_color(fb, 170, 40, 40);
    uint32_t accent = enabled ? fb_pack_color(fb, 120, 230, 170) : fb_pack_color(fb, 245, 110, 110);

    fb_fill_rect(fb, 0, 0, fb->width, fb->height, bg);

    uint32_t margin = fb->width / 12;
    if (margin < 10) margin = 10;

    uint32_t panel_h = fb->height / 3;
    if (panel_h < 40) panel_h = 40;
    fb_fill_rect(fb, margin, margin, fb->width - margin * 2, panel_h, panel);

    uint32_t bar_h = panel_h / 5;
    if (bar_h < 6) bar_h = 6;
    fb_fill_rect(fb, margin + 10, margin + panel_h + 10, fb->width - (margin + 10) * 2, bar_h, accent);
}

static inline int touch_open(touch_ctx_t *touch) {
    memset(touch, 0, sizeof(*touch));
    touch->fd = -1;

    DIR *dir = opendir("/dev/input");
    if (!dir) {
        printf("[INIT] [WARN] /dev/input not available: %s\n", strerror(errno));
        return -1;
    }

    int fallback_fd = -1;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "event", 5) != 0) {
            continue;
        }

        char path[256];
        int path_len = snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            continue;
        }

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        char name[128] = {0};
        if (ioctl(fd, EVIOCGNAME(sizeof(name)), name) < 0) {
            name[0] = '\0';
        }

        if (strcasestr(name, "touch") != NULL || strcasestr(name, "ft") != NULL || strcasestr(name, "goodix") != NULL) {
            touch->fd = fd;
            printf("[INIT] Touch input selected: %s (%s)\n", path, name[0] ? name : "unknown");
            closedir(dir);
            return 0;
        }

        if (fallback_fd < 0) {
            fallback_fd = fd;
            fd = -1;
        }

        if (fd >= 0) {
            close(fd);
        }
    }

    closedir(dir);

    if (fallback_fd >= 0) {
        touch->fd = fallback_fd;
        printf("[INIT] Touch input fallback selected: generic /dev/input/event*\n");
        return 0;
    }

    printf("[INIT] [WARN] No /dev/input/event* device available for touch polling.\n");
    return -1;
}

static inline void touch_close(touch_ctx_t *touch) {
    if (touch->fd >= 0) {
        close(touch->fd);
    }
    touch->fd = -1;
}

static inline int touch_poll_toggle(touch_ctx_t *touch, int *x, int *y) {
    if (touch->fd < 0) {
        return 0;
    }

    int toggled = 0;
    int touch_activity = 0;
    struct input_event ev;
    ssize_t n;

    while ((n = read(touch->fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_X || ev.code == ABS_MT_POSITION_X) {
                touch->last_x = ev.value;
                touch_activity = 1;
            } else if (ev.code == ABS_Y || ev.code == ABS_MT_POSITION_Y) {
                touch->last_y = ev.value;
                touch_activity = 1;
            } else if (ev.code == ABS_MT_TRACKING_ID && ev.value >= 0) {
                touch_activity = 1;
            } else if (ev.code == ABS_PRESSURE && ev.value > 0) {
                touch_activity = 1;
            }
        }

        if (ev.type == EV_KEY && ev.code == BTN_TOUCH && ev.value == 1) {
            touch_activity = 1;
        }

        if (ev.type == EV_SYN && ev.code == SYN_REPORT && touch_activity) {
            toggled = 1;
            touch_activity = 0;
        }
    }

    if (toggled) {
        if (x) *x = touch->last_x;
        if (y) *y = touch->last_y;
        return 1;
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        printf("[INIT] [WARN] Touch read error: %s\n", strerror(errno));
    }

    return 0;
}

#endif
