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
    size_t page_len;
    size_t draw_offset;
    uint32_t front_page;
    uint32_t draw_page;
    int pageflip_enabled;
    struct fb_var_screeninfo orig_vinfo;
    int has_orig_vinfo;
    char dev_path[32];
} fb_ctx_t;

typedef struct touch_ctx {
    int fd;
    int supports_mt_slots;
    int max_slots;
    int current_slot;
    int slot_active[10];
    int slot_x[10];
    int slot_y[10];
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int has_range;
} touch_ctx_t;

#define TOUCH_MAX_POINTS 10

static inline int touch_test_bit(const unsigned long *bits, unsigned int bit) {
    return (bits[bit / (8U * sizeof(unsigned long))] >> (bit % (8U * sizeof(unsigned long)))) & 1UL;
}

static inline int touch_query_bits(int fd, unsigned int ev, unsigned long *bits, size_t bits_len) {
    memset(bits, 0, bits_len);
    return ioctl(fd, EVIOCGBIT(ev, bits_len), bits);
}

static inline void touch_copy_string(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static inline int touch_is_candidate_device(int fd, int *score_out) {
    unsigned long ev_bits[(EV_MAX / (8 * sizeof(unsigned long))) + 2];
    unsigned long abs_bits[(ABS_MAX / (8 * sizeof(unsigned long))) + 2];
    unsigned long key_bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 2];
    int score = 0;

    if (touch_query_bits(fd, 0, ev_bits, sizeof(ev_bits)) < 0) {
        return 0;
    }
    if (!touch_test_bit(ev_bits, EV_ABS)) {
        return 0;
    }
    if (touch_query_bits(fd, EV_ABS, abs_bits, sizeof(abs_bits)) < 0) {
        return 0;
    }

    int has_abs_xy = touch_test_bit(abs_bits, ABS_X) && touch_test_bit(abs_bits, ABS_Y);
    int has_mt_xy = touch_test_bit(abs_bits, ABS_MT_POSITION_X) && touch_test_bit(abs_bits, ABS_MT_POSITION_Y);
    int has_mt_slot = touch_test_bit(abs_bits, ABS_MT_SLOT);
    int has_mt_tracking = touch_test_bit(abs_bits, ABS_MT_TRACKING_ID);

    if (!has_abs_xy && !has_mt_xy) {
        return 0;
    }

    if (touch_query_bits(fd, EV_KEY, key_bits, sizeof(key_bits)) == 0 && touch_test_bit(key_bits, BTN_TOUCH)) {
        score += 3;
    }
    if (has_mt_xy) {
        score += 4;
    }
    if (has_abs_xy) {
        score += 2;
    }
    if (has_mt_slot) {
        score += 2;
    }
    if (has_mt_tracking) {
        score += 2;
    }

    if (score_out) {
        *score_out = score;
    }
    return score > 0;
}

static inline int touch_probe_abs_axis(int fd, unsigned long code, int *out_min, int *out_max) {
    struct input_absinfo absinfo;
    if (ioctl(fd, EVIOCGABS(code), &absinfo) < 0) {
        return -1;
    }

    if (out_min) *out_min = absinfo.minimum;
    if (out_max) *out_max = absinfo.maximum;
    return 0;
}

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

    uint8_t *row = fb->map + fb->draw_offset + y * fb->line_length;
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
        fb->page_len = (size_t)fb->line_length * (size_t)fb->height;
        fb->draw_offset = 0;
        fb->front_page = 0;
        fb->draw_page = 0;
        fb->pageflip_enabled = 0;
        fb->orig_vinfo = vinfo;
        fb->has_orig_vinfo = 1;
        snprintf(fb->dev_path, sizeof(fb->dev_path), "%s", path);

        struct fb_var_screeninfo req = vinfo;
        if (req.yres_virtual < req.yres * 2) {
            req.yres_virtual = req.yres * 2;
            req.yoffset = 0;
            req.xoffset = 0;
            req.activate = FB_ACTIVATE_NOW;
            if (ioctl(fd, FBIOPUT_VSCREENINFO, &req) == 0) {
                struct fb_var_screeninfo now;
                if (ioctl(fd, FBIOGET_VSCREENINFO, &now) == 0 &&
                    now.yres == fb->height &&
                    now.yres_virtual >= fb->height * 2) {
                    size_t required = (size_t)fb->line_length * (size_t)fb->height * 2;
                    if (required <= fb->map_len) {
                        fb->pageflip_enabled = 1;
                        fb->draw_page = 1;
                        fb->draw_offset = fb->page_len;
                    }
                }
            }
        } else {
            size_t required = (size_t)fb->line_length * (size_t)fb->height * 2;
            if (required <= fb->map_len) {
                fb->pageflip_enabled = 1;
                fb->draw_page = 1;
                fb->draw_offset = fb->page_len;
            }
        }

        if (fb->pageflip_enabled) {
            memset(fb->map, 0, fb->page_len * 2);
            printf("[INIT] Framebuffer page-flip enabled on %s (double buffer via y-pan).\n", fb->dev_path);
        }

        printf("[INIT] Framebuffer ready on %s: %ux%u @ %ubpp\n", fb->dev_path, fb->width, fb->height, fb->bpp);
        return 0;
    }

    printf("[INIT] [WARN] No usable framebuffer found in /dev/fb0..3.\n");
    printf("[INIT] [WARN] FKMS should expose /dev/fb0 after boot settles.\n");
    return -1;
}

static inline void fb_close(fb_ctx_t *fb) {
    if (fb->fd >= 0 && fb->has_orig_vinfo) {
        struct fb_var_screeninfo restore = fb->orig_vinfo;
        restore.yoffset = 0;
        restore.xoffset = 0;
        restore.activate = FB_ACTIVATE_NOW;
        (void)ioctl(fb->fd, FBIOPUT_VSCREENINFO, &restore);
    }

    if (fb->map) {
        munmap(fb->map, fb->map_len);
        fb->map = NULL;
    }

    if (fb->fd >= 0) {
        close(fb->fd);
    }
    fb->fd = -1;
}

static inline void fb_begin_frame(fb_ctx_t *fb) {
    if (!fb) {
        return;
    }

    if (fb->pageflip_enabled) {
        fb->draw_page = (fb->front_page ^ 1u) & 1u;
        fb->draw_offset = fb->draw_page ? fb->page_len : 0;
    } else {
        fb->draw_page = 0;
        fb->draw_offset = 0;
    }
}

static inline int fb_present(fb_ctx_t *fb) {
    if (!fb || fb->fd < 0) {
        return -1;
    }

    if (!fb->pageflip_enabled) {
        return 0;
    }

    struct fb_var_screeninfo pan;
    if (ioctl(fb->fd, FBIOGET_VSCREENINFO, &pan) < 0) {
        return -1;
    }

    pan.xoffset = 0;
    pan.yoffset = fb->draw_page ? fb->height : 0;
    pan.activate = FB_ACTIVATE_VBL;

    if (ioctl(fb->fd, FBIOPAN_DISPLAY, &pan) < 0) {
        pan.activate = FB_ACTIVATE_NOW;
        if (ioctl(fb->fd, FBIOPAN_DISPLAY, &pan) < 0) {
            return -1;
        }
    }

    fb->front_page = fb->draw_page;
    return 0;
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

    int best_fd = -1;
    int best_score = -1;
    char best_path[256] = {0};
    char best_name[128] = {0};
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

        int score = 0;
        if (touch_is_candidate_device(fd, &score)) {
            if (strcasestr(name, "touch") != NULL || strcasestr(name, "ft") != NULL || strcasestr(name, "goodix") != NULL) {
                score += 4;
            }

            if (score > best_score) {
                if (best_fd >= 0) {
                    close(best_fd);
                }
                best_fd = fd;
                best_score = score;
                touch_copy_string(best_path, sizeof(best_path), path);
                touch_copy_string(best_name, sizeof(best_name), name[0] ? name : "unknown");
                fd = -1;
            }
        }

        if (fd >= 0) {
            close(fd);
        }
    }

    closedir(dir);

    if (best_fd >= 0) {
        touch->fd = best_fd;
        int ok_x = (touch_probe_abs_axis(touch->fd, ABS_X, &touch->min_x, &touch->max_x) == 0) ||
                   (touch_probe_abs_axis(touch->fd, ABS_MT_POSITION_X, &touch->min_x, &touch->max_x) == 0);
        int ok_y = (touch_probe_abs_axis(touch->fd, ABS_Y, &touch->min_y, &touch->max_y) == 0) ||
                   (touch_probe_abs_axis(touch->fd, ABS_MT_POSITION_Y, &touch->min_y, &touch->max_y) == 0);
        touch->has_range = ok_x && ok_y && (touch->max_x > touch->min_x) && (touch->max_y > touch->min_y);

        struct input_absinfo slot_info;
        if (ioctl(touch->fd, EVIOCGABS(ABS_MT_SLOT), &slot_info) == 0 && slot_info.maximum >= 0) {
            int slots = slot_info.maximum + 1;
            if (slots > TOUCH_MAX_POINTS) slots = TOUCH_MAX_POINTS;
            touch->supports_mt_slots = 1;
            touch->max_slots = slots;
        } else {
            touch->supports_mt_slots = 0;
            touch->max_slots = 1;
        }
        touch->current_slot = 0;

        printf("[INIT] Touch input selected: %s (%s)\n", best_path, best_name[0] ? best_name : "unknown");
        return 0;
    }

    printf("[INIT] [WARN] No touch-capable /dev/input/event* device available for touch polling.\n");
    return -1;
}

static inline void touch_close(touch_ctx_t *touch) {
    if (touch->fd >= 0) {
        close(touch->fd);
    }
    touch->fd = -1;
}

static inline int touch_poll_points(touch_ctx_t *touch, int *xs, int *ys, int max_points) {
    if (touch->fd < 0) {
        return -1;
    }

    struct input_event ev;
    ssize_t n;

    while ((n = read(touch->fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_SLOT && touch->supports_mt_slots) {
                if (ev.value >= 0 && ev.value < touch->max_slots) {
                    touch->current_slot = ev.value;
                }
            } else if (ev.code == ABS_X) {
                touch->slot_x[0] = ev.value;
            } else if (ev.code == ABS_Y) {
                touch->slot_y[0] = ev.value;
            } else if (ev.code == ABS_MT_POSITION_X) {
                touch->slot_x[touch->current_slot] = ev.value;
            } else if (ev.code == ABS_MT_POSITION_Y) {
                touch->slot_y[touch->current_slot] = ev.value;
            } else if (ev.code == ABS_MT_TRACKING_ID) {
                touch->slot_active[touch->current_slot] = (ev.value >= 0) ? 1 : 0;
            } else if (ev.code == ABS_PRESSURE && !touch->supports_mt_slots) {
                touch->slot_active[0] = (ev.value > 0) ? 1 : 0;
            } else if (ev.code == ABS_MT_PRESSURE && touch->supports_mt_slots) {
                if (ev.value == 0 && touch->slot_active[touch->current_slot]) {
                    touch->slot_active[touch->current_slot] = 0;
                }
            }
        }

        if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (touch->supports_mt_slots) {
                if (ev.value == 0) {
                    for (int i = 0; i < touch->max_slots; ++i) {
                        touch->slot_active[i] = 0;
                    }
                }
            } else {
                touch->slot_active[0] = (ev.value > 0) ? 1 : 0;
            }
        }
    }

    if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        printf("[INIT] [WARN] Touch read error: %s\n", strerror(errno));
    }

    int count = 0;
    int slots = touch->max_slots;
    if (slots <= 0 || slots > TOUCH_MAX_POINTS) {
        slots = TOUCH_MAX_POINTS;
    }

    for (int i = 0; i < slots && count < max_points; ++i) {
        if (!touch->slot_active[i]) {
            continue;
        }

        if (xs) xs[count] = touch->slot_x[i];
        if (ys) ys[count] = touch->slot_y[i];
        ++count;
    }

    return count;
}

#endif
