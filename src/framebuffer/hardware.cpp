#include "framebuffer/context.hpp"
#include "touch/context.hpp"

#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <glob.h>
#include <new>
#include <time.h>

static uint64_t displayMonotonicMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
}

// fbdev devices without DRM connector status (e.g. SPI panels) remain usable.
static bool displayDisconnected(int index) {
    const char *suffixes[] = { "card*-*/status", "drm/card*/card*-*/status" };
    bool disconnected = false;
    bool unknown = false;
    for (const char *suffix : suffixes) {
        char pattern[160];
        snprintf(pattern, sizeof(pattern), "/sys/class/graphics/fb%d/device/%s", index, suffix);
        glob_t paths{};
        int result = glob(pattern, 0, nullptr, &paths);
        if (result != 0 && result != GLOB_NOMATCH) {
            printf("[FBUI] [WARN] Could not enumerate connectors for fb%d: %d\n", index, result);
            unknown = true;
        }
        for (size_t i = 0; i < paths.gl_pathc; ++i) {
            FILE *file = fopen(paths.gl_pathv[i], "r");
            if (!file) {
                printf("[FBUI] [WARN] Cannot read %s: %s\n", paths.gl_pathv[i], strerror(errno));
                unknown = true;
                continue;
            }
            char status[32]{};
            bool readOk = fgets(status, sizeof(status), file) != nullptr;
            fclose(file);
            if (readOk && strcmp(status, "connected\n") == 0) {
                globfree(&paths);
                return false;
            }
            if (readOk && strcmp(status, "disconnected\n") == 0) disconnected = true;
            else unknown = true;
        }
        globfree(&paths);
    }
    return disconnected && !unknown;
}

static bool validFramebuffer(const fb_var_screeninfo &vinfo, const fb_fix_screeninfo &finfo) {
    if (vinfo.bits_per_pixel != 16 && vinfo.bits_per_pixel != 24 && vinfo.bits_per_pixel != 32) {
        return false;
    }
    return vinfo.xres > 0 && vinfo.yres > 0 &&
           (uint64_t)vinfo.xres * (vinfo.bits_per_pixel / 8) <= finfo.line_length &&
           (uint64_t)finfo.line_length * vinfo.yres <= finfo.smem_len;
}

static void initFramebuffer(FramebufferContext *fb) {
    for (int idx = 0; idx <= 3; ++idx) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/fb%d", idx);
        int fd = open(path, O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;

        struct fb_fix_screeninfo finfo{};
        struct fb_var_screeninfo vinfo{};
        if (displayDisconnected(idx)) {
            close(fd);
            continue;
        }
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
            ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0) {
            printf("[FBUI] [WARN] Cannot query %s: %s\n", path, strerror(errno));
            close(fd);
            continue;
        }
        if (!validFramebuffer(vinfo, finfo)) {
            printf("[FBUI] [WARN] Invalid framebuffer layout for %s\n", path);
            close(fd);
            continue;
        }

        fb_var_screeninfo original = vinfo;
        fb_var_screeninfo req = vinfo;
        if ((uint64_t)req.yres_virtual < (uint64_t)req.yres * 2 &&
            req.yres <= UINT32_MAX / 2) {
            req.yres_virtual = req.yres * 2;
            req.yoffset = 0;
            req.xoffset = 0;
            req.activate = FB_ACTIVATE_NOW;
            (void)ioctl(fd, FBIOPUT_VSCREENINFO, &req);
        }
        // A mode request can change both stride and memory size; map only the final layout.
        if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0 ||
            ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0 ||
            !validFramebuffer(vinfo, finfo)) {
            printf("[FBUI] [WARN] Invalid framebuffer layout for %s\n", path);
            close(fd);
            continue;
        }
        size_t pageLen = (size_t)finfo.line_length * vinfo.yres;
        uint8_t *map = static_cast<uint8_t *>(mmap(nullptr, finfo.smem_len,
                                                   PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
        if (map == MAP_FAILED) {
            printf("[FBUI] [WARN] Cannot map %s: %s\n", path, strerror(errno));
            close(fd);
            continue;
        }
        uint8_t *staging = new (std::nothrow) uint8_t[pageLen];
        if (!staging) {
            printf("[FBUI] [WARN] Cannot allocate %zu-byte framebuffer staging buffer\n", pageLen);
            munmap(map, finfo.smem_len);
            close(fd);
            continue;
        }

        fb->fd = fd;
        fb->buffer = map;
        fb->stagingBuffer = staging;
        fb->bufferSize = finfo.smem_len;
        fb->stagingBufferSize = pageLen;
        fb->width = vinfo.xres;
        fb->height = vinfo.yres;
        fb->bpp = vinfo.bits_per_pixel;
        fb->line_length = finfo.line_length;
        fb->page_len = pageLen;
        fb->front_page = vinfo.yoffset >= vinfo.yres ? 1 : 0;
        fb->pageflip_enabled = (uint64_t)vinfo.yres_virtual >= (uint64_t)vinfo.yres * 2 &&
                               pageLen <= fb->bufferSize / 2;
        fb->draw_page = fb->pageflip_enabled ? (fb->front_page ^ 1U) : 0;
        fb->draw_offset = fb->draw_page ? pageLen : 0;
        fb->orig_vinfo = original;
        fb->has_orig_vinfo = 1;
        snprintf(fb->dev_path, sizeof(fb->dev_path), "%s", path);
        memset(map, 0, fb->pageflip_enabled ? pageLen * 2 : pageLen);
        memset(staging, 0, pageLen);
        printf("[FBUI] Framebuffer %s initialized: %ux%u, page flipping %s\n",
               path, fb->width, fb->height, fb->pageflip_enabled ? "enabled" : "unavailable");
        return;
    }
}

FramebufferContext::FramebufferContext(Audiox *context)
    : app(context), fd(-1), buffer(nullptr), stagingBuffer(nullptr), bufferSize(0),
      stagingBufferSize(0), width(0), height(0), bpp(0), line_length(0), page_len(0),
      draw_offset(0), front_page(0), draw_page(0), pageflip_enabled(0),
      orig_vinfo{}, has_orig_vinfo(0), dev_path{} {
    if (app) app->fb = this;
    initFramebuffer(this);
    if (!isReady()) printf("[FBUI] No connected framebuffer; waiting for a display.\n");
    nextProbeMs = displayMonotonicMs() + AUDIOX_DEVICE_POLL_INTERVAL_MS;
}

FramebufferContext::~FramebufferContext() {
    release();
    if (app && app->fb == this) app->fb = nullptr;
}

void FramebufferContext::release() {
    resetUi();
    if (app && app->touch) app->touch->suspend();
    if (buffer) munmap(buffer, bufferSize);
    if (fd >= 0) close(fd);
    delete[] stagingBuffer;
    fd = -1;
    buffer = nullptr;
    stagingBuffer = nullptr;
    bufferSize = stagingBufferSize = 0;
    width = height = bpp = line_length = 0;
    page_len = draw_offset = 0;
    front_page = draw_page = 0;
    pageflip_enabled = has_orig_vinfo = 0;
    dev_path[0] = '\0';
}

void FramebufferContext::waitForFramebuffer(int timeoutSeconds) {
    for (int elapsed = 0; !isReady() && elapsed < timeoutSeconds * 10; ++elapsed) {
        usleep(100000);
        initFramebuffer(this);
    }
    nextProbeMs = displayMonotonicMs() + AUDIOX_DEVICE_POLL_INTERVAL_MS;
}

bool FramebufferContext::isReady() const {
    return fd >= 0 && buffer != nullptr && width > 0 && height > 0 && bpp > 0;
}

void FramebufferContext::checkForFramebuffer() {
    uint64_t now = displayMonotonicMs();
    if (now < nextProbeMs) return;
    nextProbeMs = now + AUDIOX_DEVICE_POLL_INTERVAL_MS;
    if (isReady()) {
        int index = -1;
        struct stat opened{}, current{};
        fb_var_screeninfo vinfo{};
        fb_fix_screeninfo finfo{};
        bool unchanged = sscanf(dev_path, "/dev/fb%d", &index) == 1 &&
            !displayDisconnected(index) &&
            fstat(fd, &opened) == 0 && stat(dev_path, &current) == 0 &&
            opened.st_dev == current.st_dev && opened.st_ino == current.st_ino &&
            opened.st_rdev == current.st_rdev &&
            ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) == 0 &&
            ioctl(fd, FBIOGET_FSCREENINFO, &finfo) == 0 &&
            validFramebuffer(vinfo, finfo) &&
            vinfo.xres == width && vinfo.yres == height && vinfo.bits_per_pixel == bpp &&
            finfo.line_length == line_length && finfo.smem_len == bufferSize &&
            (!pageflip_enabled || (uint64_t)vinfo.yres_virtual >= (uint64_t)height * 2);
        if (unchanged) return;
        printf("[FBUI] Framebuffer %s disconnected or changed; releasing display resources.\n", dev_path);
        release();
    }
    initFramebuffer(this);
}
