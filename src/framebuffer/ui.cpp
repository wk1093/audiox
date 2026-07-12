#include "framebuffer/context.hpp"

#include "touch/context.hpp"

#include <stdio.h>
#include <time.h>
#include <cmath>

static uint8_t* boot_logo_data = nullptr;
static constexpr int kButtonDragThresholdPx = 12;

// Audio level bar state for placeholder visualization
struct AudioLevelState {
    float levels[2]{0.0f, 0.0f};  // stereo levels
    float targetLevels[2]{0.0f, 0.0f};
    float decayRate = 0.92f;
    int animationCounter = 0;
};

static AudioLevelState audioLevelState;

static inline int calculateLogoY(int screenHeight, int logoHeight) {
    const int textHeight = 8;
    return (screenHeight - (logoHeight + textHeight + 16)) / 2;
}

static inline int calculateTextY(int screenHeight, int textHeight, int logoHeight) {
    return calculateLogoY(screenHeight, logoHeight) + logoHeight + 16;
}

static int _calculatedLogoHeight = 0;

static inline bool pointInRect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static inline void drawLine(FramebufferContext *fb, int x1, int y1, int x2, int y2, 
                            unsigned char r, unsigned char g, unsigned char b) {
    // Simple line drawing using Bresenham-like approach
    int dx = x2 - x1;
    int dy = y2 - y1;
    int steps = (dx > dy) ? dx : dy;
    if (steps < 1) steps = 1;
    
    for (int i = 0; i <= steps; i++) {
        int x = x1 + (dx * i) / steps;
        int y = y1 + (dy * i) / steps;
        fb->drawPixel(x, y, r, g, b);
    }
}

static inline void drawRectBorder(FramebufferContext *fb, int x, int y, int w, int h,
                                   unsigned char r, unsigned char g, unsigned char b, int thickness = 1) {
    // Draw filled rectangle first for simplicity
    for (int i = 0; i < thickness; i++) {
        drawLine(fb, x + i, y + i, x + w - i, y + i, r, g, b);
        drawLine(fb, x + i, y + h - i - 1, x + w - i, y + h - i - 1, r, g, b);
        drawLine(fb, x + i, y + i, x + i, y + h - i, r, g, b);
        drawLine(fb, x + w - i - 1, y + i, x + w - i - 1, y + h - i, r, g, b);
    }
}

static inline void updateAudioLevels() {
    // Placeholder audio level animation - simulates meter decay and random peaks
    audioLevelState.animationCounter++;
    
    // Every few frames, generate a new target
    if (audioLevelState.animationCounter % 100 == 0) {
        audioLevelState.targetLevels[0] = 0.2f + (float)(rand() % 80) / 100.0f;
        audioLevelState.targetLevels[1] = 0.2f + (float)(rand() % 80) / 100.0f;
    }
    
    // Smooth approach to target, then decay
    for (int i = 0; i < 2; i++) {
        if (audioLevelState.levels[i] < audioLevelState.targetLevels[i]) {
            audioLevelState.levels[i] += 
                (audioLevelState.targetLevels[i] - audioLevelState.levels[i]) * 0.15f;
        } else {
            audioLevelState.levels[i] *= audioLevelState.decayRate;
        }
        if (audioLevelState.levels[i] < 0.01f) audioLevelState.levels[i] = 0.0f;
    }
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

    beginFrame();

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

    present();

}

struct ButtonState {
    int id;
    bool wasTouchDown{false};
    bool tracking{false};
    bool dragged{false};
    int startX{0};
    int startY{0};
    int lastX{0};
    int lastY{0};
};

static ButtonState buttonStates[10];

static inline int imuiButton(FramebufferContext *fb, int buttonId, int x, int y, int w, int h, 
                              const char *label, TouchState *touch) {
    if (buttonId >= 10) return 0;
    
    ButtonState &interaction = buttonStates[buttonId];
    interaction.id = buttonId;
    
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

    // Draw button with border
    if (isPressed) {
        fb->drawRect(x, y, w, h, 80, 120, 160);
        drawRectBorder(fb, x, y, w, h, 100, 150, 200, 2);
    } else {
        fb->drawRect(x, y, w, h, 60, 80, 110);
        drawRectBorder(fb, x, y, w, h, 120, 140, 170, 1);
    }

    // Draw label centered
    int text_w = 0;
    for (const char *p = label; *p; ++p) {
        unsigned char c = (unsigned char)(*p);
        if (c >= 128) continue;
        text_w += 8;
    }
    int text_x = x + (w - text_w) / 2;
    int text_y = y + (h - 8) / 2;

    fb->drawText(text_x, text_y, label, 255, 255, 255, 1);

    return activated ? 1 : 0;
}



void FramebufferContext::drawMain(TouchState *touchState) {
    if (!isReady()) {
        return;
    }

    updateAudioLevels();
    beginFrame();

    // Background
    drawRect(0, 0, width, height, 15, 20, 30);

    int padding = 10;
    int statusHeight = 60;
    int levelBarHeight = 100;
    int buttonHeight = 45;
    int buttonCount = 3;

    // Status panel - top
    drawRect(padding, padding, width - 2*padding, statusHeight, 30, 40, 60);
    drawRectBorder(this, padding, padding, width - 2*padding, statusHeight, 100, 140, 180, 1);

    drawText(padding + 5, padding + 5, "Audio:", 150, 188, 228, 1);
    drawText(padding + 5, padding + 20, "44.1kHz | Ready", 200, 220, 255, 1);

    // Audio level bars - middle
    int levelAreaY = padding + statusHeight + 15;
    int levelAreaHeight = levelBarHeight;
    
    drawRect(padding, levelAreaY, width - 2*padding, levelAreaHeight, 25, 35, 50);
    drawRectBorder(this, padding, levelAreaY, width - 2*padding, levelAreaHeight, 80, 120, 160, 1);

    // Draw level labels
    drawText(padding + 5, levelAreaY + 5, "Level", 150, 188, 228, 1);

    // Draw stereo level bars
    int barY = levelAreaY + 20;
    int barH = 15;
    int barSpacing = 5;
    
    for (int ch = 0; ch < 2; ch++) {
        int barX = padding + 10;
        int barMaxW = width - 2*padding - 20;
        
        // Channel label
        if (ch == 0) drawText(barX - 5, barY - 2, "L", 150, 188, 228, 1);
        else drawText(barX - 5, barY + barH + barSpacing - 2, "R", 150, 188, 228, 1);
        
        // Background bar
        drawRect(barX, barY + ch * (barH + barSpacing), barMaxW, barH, 40, 50, 70);
        drawRectBorder(this, barX, barY + ch * (barH + barSpacing), barMaxW, barH, 80, 100, 120, 1);
        
        // Level indicator
        int levelW = (int)(barMaxW * audioLevelState.levels[ch]);
        if (levelW > 0) {
            // Color gradient: green -> yellow -> red based on level
            unsigned char r, g, b;
            float level = audioLevelState.levels[ch];
            if (level < 0.5f) {
                r = (unsigned char)(level * 2 * 100);
                g = 200;
                b = (unsigned char)(level * 2 * 50);
            } else if (level < 0.8f) {
                r = 200;
                g = (unsigned char)(200 - (level - 0.5f) * 2 * 100);
                b = 0;
            } else {
                r = 255;
                g = (unsigned char)(100 - (level - 0.8f) * 5 * 100);
                b = 0;
            }
            drawRect(barX, barY + ch * (barH + barSpacing), levelW, barH, r, g, b);
        }
    }

    // Control buttons - bottom
    int buttonsY = height - padding - buttonHeight - 5;
    int buttonW = (width - 4*padding) / buttonCount;
    
    for (int i = 0; i < buttonCount; i++) {
        int btnX = padding + i * (buttonW + padding);
        const char *btnLabel = nullptr;
        
        switch (i) {
            case 0: btnLabel = "Reset"; break;
            case 1: btnLabel = "Info"; break;
            case 2: btnLabel = "Menu"; break;
        }
        
        int pressed = imuiButton(this, i, btnX, buttonsY, buttonW, buttonHeight, btnLabel, touchState);
        if (pressed) {
            // TODO: Handle button presses
        }
    }

    present();
}