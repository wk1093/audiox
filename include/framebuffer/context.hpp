#pragma once

#include <cstddef>
#include <cstdint>
#include "defs.hpp"
#include "../context.hpp"
#include <linux/fb.h>

struct TouchState;

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

    FramebufferContext(Audiox *context);

    // Hardware-related functions
    void waitForFramebuffer(int timeoutSeconds);
    bool isReady() const WARN_UNUSED;
    void checkForFramebuffer();
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