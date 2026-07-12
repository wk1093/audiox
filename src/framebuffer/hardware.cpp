#include "framebuffer/context.hpp"

#include <unistd.h>
#include <linux/fb.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>

void initFramebuffer(FramebufferContext *fb) {
    // We have this separated because the constructor and the poll method might need to do this
    // for example if the screen isn't plugged in yet, we can check it for later
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
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 || ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
            close(fd);
            continue;
        }

        if (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 24 && vinfo.bits_per_pixel != 32) {
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
        fb->buffer = map;
        if (!fb->stagingBuffer || fb->stagingBufferSize != (size_t)finfo.line_length * (size_t)vinfo.yres) {
            delete[] fb->stagingBuffer;
            fb->stagingBuffer = new uint8_t[(size_t)finfo.line_length * (size_t)vinfo.yres];
            fb->stagingBufferSize = (size_t)finfo.line_length * (size_t)vinfo.yres;
        }
        fb->bufferSize = finfo.smem_len;
        fb->width = vinfo.xres;
        fb->height = vinfo.yres;
        fb->bpp = vinfo.bits_per_pixel;
        fb->line_length = finfo.line_length;
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
                    if (required <= fb->bufferSize) {
                        fb->pageflip_enabled = 1;
                        fb->draw_page = 1;
                        fb->draw_offset = fb->page_len;
                    }
                }
            }
        } else {
            size_t required = (size_t)fb->line_length * (size_t)fb->height * 2;
            if (required <= fb->bufferSize) {
                fb->pageflip_enabled = 1;
                fb->draw_page = 1;
                fb->draw_offset = fb->page_len;
            }
        }

        if (fb->pageflip_enabled) {
            memset(fb->buffer, 0, fb->page_len * 2);
            printf("[INIT] framebuffer %s page flipping enabled\n", path);
        } else {
            memset(fb->buffer, 0, fb->page_len);
            printf("[INIT] framebuffer %s page flipping not available\n", path);
        }
        memset(fb->stagingBuffer, 0, fb->page_len);
        return;
    }
    printf("[INIT] [WARN] No usable framebuffer found in /dev/fb0..3.\n");
}

FramebufferContext::FramebufferContext(Audiox *context) {
    app = context;

    fd = -1;
    buffer = nullptr;
    stagingBuffer = nullptr;
    bufferSize = 0;
    stagingBufferSize = 0;
    width = 0;
    height = 0;
    bpp = 0;
    line_length = 0;
    page_len = 0;
    draw_offset = 0;
    front_page = 0;
    draw_page = 0;
    pageflip_enabled = 0;
    has_orig_vinfo = 0;
    memset(&orig_vinfo, 0, sizeof(orig_vinfo));
    memset(dev_path, 0, sizeof(dev_path));

    initFramebuffer(this);
    
    
}

void FramebufferContext::waitForFramebuffer(int timeoutSeconds) {
    // just calls checkForFramebuffer() in a loop until the framebuffer is ready or timeout occurs
    int elapsed = 0;
    while (!isReady() && elapsed < timeoutSeconds * 10) {
        checkForFramebuffer();
        usleep(100000); // 100ms
        elapsed++;
    }
}

bool FramebufferContext::isReady() const {
    return fd >= 0 && buffer != nullptr && width > 0 && height > 0 && bpp > 0;
}

void FramebufferContext::checkForFramebuffer() {
    if (!isReady()) {
        initFramebuffer(this);
    }
}