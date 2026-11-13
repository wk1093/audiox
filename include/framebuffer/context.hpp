#pragma once

#include <cstddef>
#include <cstdint>
#include "defs.hpp"
#include "../context.hpp"
#include <linux/fb.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>

struct TouchState;

enum class DisplayBackend : uint8_t { None, Fbdev, Kms };

// Native DRM/KMS output: two dumb buffers page-flipped on one CRTC. The DRM fd
// is the DRM master, so the kernel's fbdev emulation/console stays off-screen.
struct KmsDisplay {
    int fd{-1};
    int card{-1};
    uint32_t connectorId{0};
    uint32_t connectorType{0};
    uint32_t crtcId{0};
    uint32_t fbIds[2]{};
    uint32_t handles[2]{};
    uint8_t *maps[2]{};
    size_t mapSize{0};
    drm_mode_modeinfo mode{};
    bool flipPending{false};
    bool flipTimeoutWarned{false};
    bool setupWarned{false};
    // A KMS device exists but none of its connectors has a display.
    bool idleCard{false};
    char name[32]{};
};

struct FramebufferContext {
    Audiox *app;
    int fd;
    uint8_t *buffer;
    uint8_t *stagingBuffer;
    size_t bufferSize;
    size_t stagingBufferSize;
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
    uint64_t nextProbeMs{0};
    DisplayBackend backend{DisplayBackend::None};
    KmsDisplay kms{};
    // Set by DRM uevents (hotplug, new card): re-probe connectors on next check.
    bool displayEventPending{false};

    FramebufferContext(Audiox *context);
    ~FramebufferContext();
    FramebufferContext(const FramebufferContext &) = delete;
    FramebufferContext &operator=(const FramebufferContext &) = delete;

    // Hardware-related functions
    void waitForFramebuffer(int timeoutSeconds);
    bool isReady() const WARN_UNUSED {
        return fd >= 0 && buffer != nullptr && width > 0 && height > 0 && bpp > 0;
    }
    bool usingKms() const { return backend == DisplayBackend::Kms && isReady(); }
    // Called for DRM uevents; the next checkForFramebuffer() re-probes at once.
    void displayEvent() { displayEventPending = true; }
    void checkForFramebuffer();
    bool initDisplay(bool probe);
    bool initFbdev();
    bool initKms(bool probe);
    bool kmsOutputValid(bool probe);
    bool kmsDisplayAvailable();
    bool kmsPresent();
    void kmsWaitFlip(int timeoutMs);
    void releaseKms();
    void release();
    void resetUi();
    void drawPixel(int x, int y, unsigned char r, unsigned char g, unsigned char b);

    // Low-level drawing functions
    void beginFrame();
    void drawRect(int x, int y, int w, int h, unsigned char r, unsigned char g, unsigned char b);
    void drawImage(int x, int y, int w, int h, const unsigned char *data);
    void drawText(int x, int y, const char *text, unsigned char r, unsigned char g, unsigned char b, int size);
    void present();

    // High-level drawing functions
    void drawBootLogo();
    void bootStatus(const char *status);
    void drawMain(TouchState *touchState);
};