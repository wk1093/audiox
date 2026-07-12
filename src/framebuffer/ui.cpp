#include "framebuffer/context.hpp"

#include "touch/context.hpp"

#include <stdio.h>

static uint8_t* boot_logo_data = nullptr;
static constexpr int kButtonDragThresholdPx = 12;

static inline int calculateLogoY(int screenHeight, int logoHeight) {
    const int textHeight = 8;
    return (screenHeight - (logoHeight + textHeight + 16)) / 2;
}

static inline int calculateTextY(int screenHeight, int textHeight, int logoHeight) {
    return calculateLogoY(screenHeight, logoHeight) + logoHeight + 16;
}

static int _calculatedLogoHeight = 0;

struct ButtonInteractionState {
    bool wasTouchDown{false};
    bool tracking{false};
    bool dragged{false};
    int startX{0};
    int startY{0};
    int lastX{0};
    int lastY{0};
};

static inline bool pointInRect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

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

// IMUI implementation

static inline bool imuiButton(FramebufferContext *fb, int x, int y, int w, int h, const char *label, TouchState *touch) {
    static ButtonInteractionState interaction;

    const bool touchDown = touch && touch->is_pressed;
    const int touchX = touch ? touch->x : interaction.lastX;
    const int touchY = touch ? touch->y : interaction.lastY;
    bool activated = false;

    if (touchDown) {
        interaction.lastX = touchX;
        interaction.lastY = touchY;
    }

    if (touchDown && !interaction.wasTouchDown && pointInRect(touchX, touchY, x, y, w, h)) {
        interaction.tracking = true;
        interaction.dragged = false;
        interaction.startX = touchX;
        interaction.startY = touchY;
    } else if (touchDown && interaction.tracking) {
        const int dx = touchX - interaction.startX;
        const int dy = touchY - interaction.startY;
        if ((dx * dx) + (dy * dy) > (kButtonDragThresholdPx * kButtonDragThresholdPx) ||
            !pointInRect(touchX, touchY, x, y, w, h)) {
            interaction.dragged = true;
        }
    } else if (!touchDown && interaction.wasTouchDown) {
        if (interaction.tracking && !interaction.dragged &&
            pointInRect(interaction.lastX, interaction.lastY, x, y, w, h)) {
            activated = true;
        }
        interaction.tracking = false;
        interaction.dragged = false;
    }

    interaction.wasTouchDown = touchDown;

    const bool isPressed = touchDown && interaction.tracking && !interaction.dragged;

    // draw button
    if (isPressed) {
        fb->drawRect(x, y, w, h, 100, 100, 100);
    } else {
        fb->drawRect(x, y, w, h, 200, 200, 200);
    }

    // draw label centered
    int text_w = 0;
    for (const char *p = label; *p; ++p) {
        unsigned char c = (unsigned char)(*p);
        if (c >= 128) {
            continue;
        }
        text_w += 8;
    }
    int text_x = x + (w - text_w) / 2;
    int text_y = y + (h - 8) / 2;

    fb->drawText(text_x, text_y, label, 0, 0, 0, 1);

    return activated;
}



void FramebufferContext::drawMain(TouchState *touchState) {
    if (!isReady()) {
        return;
    }

    // draw background
    drawRect(0, 0, width, height, 8, 11, 16);

    // draw a button in the center of the screen
    int button_w = 200;
    int button_h = 50;
    int button_x = (width - button_w) / 2;
    int button_y = (height - button_h) / 2;

    bool buttonPressed = imuiButton(this, button_x, button_y, button_w, button_h, "Click Me", touchState);

    if (buttonPressed) {
        drawText(button_x + 10, button_y + button_h + 10, "Button Pressed!", 255, 255, 255, 1);
    }
}