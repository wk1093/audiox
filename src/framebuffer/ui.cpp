#include "framebuffer/context.hpp"

#include <stdio.h>

static uint8_t* boot_logo_data = nullptr;

static inline int calculateLogoY(int screenHeight, int logoHeight) {
    const int textHeight = 8;
    return (screenHeight - (logoHeight + textHeight + 16)) / 2;
}

static inline int calculateTextY(int screenHeight, int textHeight, int logoHeight) {
    return calculateLogoY(screenHeight, logoHeight) + logoHeight + 16;
}

static int _calculatedLogoHeight = 0;

void FramebufferContext::drawBootLogo() {
    if (!isReady()) {
        return;
    }

    if (!boot_logo_data) {
        // load boot logo from file
        FILE *f = fopen(BOOT_LOGO_PPM_PATH, "rb");
        if (!f) {
            return;
        }

        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);

        boot_logo_data = new uint8_t[size];
        if (fread(boot_logo_data, 1, size, f) != (size_t)size) {
            delete[] boot_logo_data;
            boot_logo_data = nullptr;
            fclose(f);
            return;
        }
        fclose(f);
    }

    // draw boot logo centered
    int img_w = 0;
    int img_h = 0;
    const char *pixel_data = (const char *)boot_logo_data;
    if (sscanf(pixel_data, "P6\n%d %d\n255\n", &img_w, &img_h) != 2) {
        return;
    }

    int x = (width - img_w) / 2;
    int y = calculateLogoY(height, img_h);
    if (_calculatedLogoHeight != img_h) {
        _calculatedLogoHeight = img_h;
    }
    
    // fill bg with a nice gray color
    drawRect(0, 0, width, height, 8, 11, 16);

    drawImage(x, y, img_w, img_h, (const unsigned char *)boot_logo_data);
}

void FramebufferContext::bootStatus(const char *status) {
    if (!isReady()) {
        return;
    }

    printf("[INIT] [INFO] BOOT: %s\n", status);

    // draw boot logo
    drawBootLogo();

    // draw status text centered below the logo
    int text_w = 0;
    int text_h = 0;
    for (const char *p = status; *p; ++p) {
        unsigned char c = (unsigned char)(*p);
        if (c >= 128) {
            continue;
        }
        text_w += 8;
        if (text_h < 8) {
            text_h = 8;
        }
    }

    int x = (width - text_w) / 2;
    int y = calculateTextY(height, text_h, _calculatedLogoHeight);

    drawText(x, y, status, 150, 188, 228, 1);

}

void FramebufferContext::drawMain(TouchEvent *touchEvent) {
    // TODO
    drawText(10, 10, "AUDIOX", 120, 230, 170, 3);
}