#include "framebuffer/context.hpp"

#include "audio/context.hpp"
#include "config/context.hpp"
#include "touch/context.hpp"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include <cmath>
#include <errno.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/reboot.h>

static uint8_t* boot_logo_data = nullptr;
static constexpr int kButtonDragThresholdPx = 12;
static constexpr int kMaxUiDevices = 32;
static constexpr int kChannelMeterMax = 16;
static constexpr int kMeterSmoothingSlots = kMaxUiDevices * kChannelMeterMax * 2;
static constexpr float kMeterAttackTauMs = 45.0f;
static constexpr float kMeterReleaseTauMs = 180.0f;
static constexpr uint64_t kMeterSlotStaleMs = 3000ULL;

struct SystemMetricsState {
    uint64_t lastUpdateMs{0};
    uint64_t prevTotalTicks{0};
    uint64_t prevIdleTicks{0};
    uint64_t memTotalKb{0};
    uint64_t memUsedKb{0};
    float cpuUsage{0.0f};
    float ramUsage{0.0f};
    bool hasCpuSample{false};
};

struct ScrollAreaState {
    bool wasTouchDown{false};
    bool tracking{false};
    bool dragged{false};
    int startX{0};
    int startY{0};
    int lastX{0};
    int lastY{0};
    int startOffset{0};
    int offset{0};
};

struct MeterSmoothingState {
    AudioHandle handle{0};
    uint8_t channelIndex{0};
    uint8_t isCapture{0};
    float level{0.0f};
    uint64_t lastUpdateMs{0};
    uint64_t lastSeenMs{0};
    bool active{false};
};

static SystemMetricsState systemMetricsState;
static ScrollAreaState deviceListScrollState;
static MeterSmoothingState meterSmoothingState[kMeterSmoothingSlots];
static int _calculatedLogoHeight = 0;
static bool uiSleepEnabled = false;
static bool uiSleepFrameDrawn = false;
static bool uiSleepLastTouchDown = false;

static inline int requestRebootCommand(int cmd) {
    sync();
    return (int)syscall(SYS_reboot,
                        (long)LINUX_REBOOT_MAGIC1,
                        (long)LINUX_REBOOT_MAGIC2,
                        (long)cmd,
                        0L);
}

static inline void requestSystemReboot() {
    if (requestRebootCommand(LINUX_REBOOT_CMD_RESTART) != 0) {
        printf("[FBUI] [WARN] Reboot syscall failed: %s\n", strerror(errno));
    }
}

static inline void requestSystemPoweroff() {
    if (requestRebootCommand(LINUX_REBOOT_CMD_POWER_OFF) != 0) {
        printf("[FBUI] [WARN] Shutdown syscall failed: %s\n", strerror(errno));
    }
}

static inline int calculateLogoY(int screenHeight, int logoHeight) {
    const int textHeight = 8;
    return (screenHeight - (logoHeight + textHeight + 16)) / 2;
}

static inline int calculateTextY(int screenHeight, int textHeight, int logoHeight) {
    (void)textHeight;
    return calculateLogoY(screenHeight, logoHeight) + logoHeight + 16;
}

static inline bool pointInRect(int px, int py, int x, int y, int w, int h) {
    return px >= x && px < x + w && py >= y && py < y + h;
}

static inline int clampInt(int value, int minValue, int maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

static inline float clampFloat(float value, float minValue, float maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

static inline uint64_t uiMonotonicMs() {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }

    return ((uint64_t)ts.tv_sec * 1000ULL) + ((uint64_t)ts.tv_nsec / 1000000ULL);
}

static inline void drawLine(FramebufferContext *fb,
                            int x1,
                            int y1,
                            int x2,
                            int y2,
                            unsigned char r,
                            unsigned char g,
                            unsigned char b) {
    int dx = x2 - x1;
    int dy = y2 - y1;
    int absDx = dx < 0 ? -dx : dx;
    int absDy = dy < 0 ? -dy : dy;
    int steps = absDx > absDy ? absDx : absDy;
    if (steps < 1) {
        steps = 1;
    }

    for (int i = 0; i <= steps; ++i) {
        int x = x1 + (dx * i) / steps;
        int y = y1 + (dy * i) / steps;
        fb->drawPixel(x, y, r, g, b);
    }
}

static inline void drawRectBorder(FramebufferContext *fb,
                                  int x,
                                  int y,
                                  int w,
                                  int h,
                                  unsigned char r,
                                  unsigned char g,
                                  unsigned char b,
                                  int thickness = 1) {
    for (int i = 0; i < thickness; ++i) {
        drawLine(fb, x + i, y + i, x + w - i, y + i, r, g, b);
        drawLine(fb, x + i, y + h - i - 1, x + w - i, y + h - i - 1, r, g, b);
        drawLine(fb, x + i, y + i, x + i, y + h - i, r, g, b);
        drawLine(fb, x + w - i - 1, y + i, x + w - i - 1, y + h - i, r, g, b);
    }
}

static inline void drawMeterFill(FramebufferContext *fb, int x, int y, int w, int h, float level) {
    float clamped = clampFloat(level, 0.0f, 1.0f);
    int fillW = (int)((float)w * clamped);
    if (fillW <= 0) {
        return;
    }

    unsigned char r = 64;
    unsigned char g = 190;
    unsigned char b = 80;
    if (clamped >= 0.80f) {
        r = 235;
        g = 72;
        b = 60;
    } else if (clamped >= 0.60f) {
        r = 220;
        g = 174;
        b = 44;
    }

    fb->drawRect(x, y, fillW, h, r, g, b);
}

static inline void drawMeterFillColor(FramebufferContext *fb,
                                      int x,
                                      int y,
                                      int w,
                                      int h,
                                      float level,
                                      unsigned char r,
                                      unsigned char g,
                                      unsigned char b) {
    float clamped = clampFloat(level, 0.0f, 1.0f);
    int fillW = (int)((float)w * clamped);
    if (fillW <= 0) {
        return;
    }

    fb->drawRect(x, y, fillW, h, r, g, b);
}

static inline void trimLabelToWidth(const char *src, char *dst, size_t dstSize, int maxChars) {
    if (!dst || dstSize == 0) {
        return;
    }

    dst[0] = '\0';
    if (!src || maxChars <= 0) {
        return;
    }

    int srcLen = (int)strnlen(src, dstSize - 1);
    if (srcLen <= maxChars) {
        snprintf(dst, dstSize, "%s", src);
        return;
    }

    int copyChars = maxChars;
    if (copyChars > (int)dstSize - 1) {
        copyChars = (int)dstSize - 1;
    }
    if (copyChars <= 3) {
        copyChars = 0;
    } else {
        copyChars -= 3;
    }

    if (copyChars > 0) {
        memcpy(dst, src, (size_t)copyChars);
    }
    dst[copyChars] = '\0';
    snprintf(dst + copyChars, dstSize - (size_t)copyChars, "...");
}

static inline void sortDeviceInfos(AudioDeviceInfo *infos, size_t count) {
    for (size_t i = 1; i < count; ++i) {
        AudioDeviceInfo key = infos[i];
        size_t j = i;
        while (j > 0) {
            const AudioDeviceInfo &prev = infos[j - 1];
            bool shouldSwap = false;
            if (prev.cardIndex > key.cardIndex) {
                shouldSwap = true;
            } else if (prev.cardIndex == key.cardIndex && prev.deviceIndex > key.deviceIndex) {
                shouldSwap = true;
            }
            if (!shouldSwap) {
                break;
            }
            infos[j] = infos[j - 1];
            --j;
        }
        infos[j] = key;
    }
}

static inline int channelCountForUi(uint8_t channels, uint8_t hasDirection) {
    if (!hasDirection) {
        return 0;
    }
    int count = channels > 0 ? (int)channels : 2;
    if (count > kChannelMeterMax) {
        count = kChannelMeterMax;
    }
    return count;
}

static inline int deviceRowHeight(const AudioDeviceInfo &info) {
    const int meterLabelH = 8;
    const int meterBarH = 2;
    const int meterGap = 2;
    int playbackCount = channelCountForUi(info.playbackChannels, info.hasPlayback);
    int captureCount = channelCountForUi(info.captureChannels, info.hasCapture);
    int maxChannels = playbackCount > captureCount ? playbackCount : captureCount;
    int meterHeight = 0;
    if (maxChannels > 0) {
        meterHeight = meterLabelH + 3 + (maxChannels * meterBarH) + ((maxChannels - 1) * meterGap);
    }
    int textHeight = 24;
    int innerHeight = meterHeight > textHeight ? meterHeight : textHeight;
    return innerHeight + 8;
}

static inline float getAudioChannelLevel(FramebufferContext *fb,
                                         AudioHandle handle,
                                         int channelIndex,
                                         int isCapture) {
    if (!fb || !fb->app || !fb->app->audio) {
        return 0.0f;
    }
    
    return fb->app->audio->getChannelLevel(handle, channelIndex, isCapture);
}

static inline MeterSmoothingState *findMeterSmoothingState(AudioHandle handle,
                                                           int channelIndex,
                                                           int isCapture,
                                                           uint64_t nowMs) {
    MeterSmoothingState *reuse = nullptr;
    uint64_t oldestSeenMs = UINT64_MAX;

    for (int i = 0; i < kMeterSmoothingSlots; ++i) {
        MeterSmoothingState *state = &meterSmoothingState[i];
        if (state->active &&
            state->handle == handle &&
            state->channelIndex == (uint8_t)channelIndex &&
            state->isCapture == (uint8_t)(isCapture ? 1 : 0)) {
            return state;
        }

        if (!reuse) {
            if (!state->active) {
                reuse = state;
                continue;
            }
            if ((nowMs - state->lastSeenMs) > kMeterSlotStaleMs) {
                reuse = state;
                continue;
            }
        }

        if (state->lastSeenMs < oldestSeenMs) {
            oldestSeenMs = state->lastSeenMs;
            if (!reuse) {
                reuse = state;
            }
        }
    }

    if (!reuse) {
        return nullptr;
    }

    reuse->handle = handle;
    reuse->channelIndex = (uint8_t)channelIndex;
    reuse->isCapture = (uint8_t)(isCapture ? 1 : 0);
    reuse->level = 0.0f;
    reuse->lastUpdateMs = 0;
    reuse->lastSeenMs = nowMs;
    reuse->active = true;
    return reuse;
}

static inline float smoothMeterLevel(AudioHandle handle,
                                     int channelIndex,
                                     int isCapture,
                                     float targetLevel,
                                     uint64_t nowMs) {
    MeterSmoothingState *state = findMeterSmoothingState(handle, channelIndex, isCapture, nowMs);
    float clampedTarget = clampFloat(targetLevel, 0.0f, 1.0f);
    if (!state) {
        return clampedTarget;
    }

    if (state->lastUpdateMs == 0 || nowMs <= state->lastUpdateMs) {
        state->level = clampedTarget;
    } else {
        float dtMs = (float)(nowMs - state->lastUpdateMs);
        float tauMs = clampedTarget >= state->level ? kMeterAttackTauMs : kMeterReleaseTauMs;
        float alpha = 1.0f - expf(-dtMs / tauMs);
        state->level += (clampedTarget - state->level) * alpha;
    }

    state->level = clampFloat(state->level, 0.0f, 1.0f);
    state->lastUpdateMs = nowMs;
    state->lastSeenMs = nowMs;
    return state->level;
}

static inline void drawChannelMeterGroup(FramebufferContext *fb,
                                         int x,
                                         int y,
                                         int w,
                                         int channelCount,
                                         const char *label,
                                         AudioHandle handle,
                                         int isCapture,
                                         uint64_t nowMs,
                                         unsigned char fillR,
                                         unsigned char fillG,
                                         unsigned char fillB) {
    if (channelCount <= 0) {
        return;
    }

    const int meterBarH = 2;
    const int meterGap = 2;
    fb->drawText(x, y, label, 150, 188, 228, 1);
    int barsY = y + 11;
    for (int ch = 0; ch < channelCount; ++ch) {
        int barY = barsY + (ch * (meterBarH + meterGap));
        fb->drawRect(x, barY, w, meterBarH, 42, 52, 68);
        float level = getAudioChannelLevel(fb, handle, ch, isCapture);
        level = smoothMeterLevel(handle, ch, isCapture, level, nowMs);
        drawMeterFillColor(fb, x, barY, w, meterBarH, level, fillR, fillG, fillB);
    }
}

static inline void refreshSystemMetrics() {
    uint64_t nowMs = uiMonotonicMs();
    if (systemMetricsState.lastUpdateMs != 0 && (nowMs - systemMetricsState.lastUpdateMs) < 250ULL) {
        return;
    }
    systemMetricsState.lastUpdateMs = nowMs;

    FILE *statFile = fopen("/proc/stat", "r");
    if (statFile) {
        char line[256];
        if (fgets(line, sizeof(line), statFile)) {
            unsigned long long user = 0;
            unsigned long long nice = 0;
            unsigned long long system = 0;
            unsigned long long idle = 0;
            unsigned long long iowait = 0;
            unsigned long long irq = 0;
            unsigned long long softirq = 0;
            unsigned long long steal = 0;
            if (sscanf(line,
                       "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                       &user,
                       &nice,
                       &system,
                       &idle,
                       &iowait,
                       &irq,
                       &softirq,
                       &steal) >= 4) {
                uint64_t idleTicks = idle + iowait;
                uint64_t totalTicks = user + nice + system + idle + iowait + irq + softirq + steal;
                if (systemMetricsState.hasCpuSample && totalTicks > systemMetricsState.prevTotalTicks) {
                    uint64_t deltaTotal = totalTicks - systemMetricsState.prevTotalTicks;
                    uint64_t deltaIdle = 0;
                    if (idleTicks > systemMetricsState.prevIdleTicks) {
                        deltaIdle = idleTicks - systemMetricsState.prevIdleTicks;
                    }

                    if (deltaTotal > 0 && deltaIdle <= deltaTotal) {
                        float busy = (float)(deltaTotal - deltaIdle) / (float)deltaTotal;
                        systemMetricsState.cpuUsage = clampFloat(busy, 0.0f, 1.0f);
                    }
                }
                systemMetricsState.prevIdleTicks = idleTicks;
                systemMetricsState.prevTotalTicks = totalTicks;
                systemMetricsState.hasCpuSample = true;
            }
        }
        fclose(statFile);
    }

    FILE *memFile = fopen("/proc/meminfo", "r");
    if (memFile) {
        char line[256];
        uint64_t memTotalKb = 0;
        uint64_t memAvailableKb = 0;
        uint64_t memFreeKb = 0;
        while (fgets(line, sizeof(line), memFile)) {
            unsigned long long value = 0;
            if (sscanf(line, "MemTotal: %llu kB", &value) == 1) {
                memTotalKb = value;
            } else if (sscanf(line, "MemAvailable: %llu kB", &value) == 1) {
                memAvailableKb = value;
            } else if (sscanf(line, "MemFree: %llu kB", &value) == 1) {
                memFreeKb = value;
            }
        }
        fclose(memFile);

        if (memTotalKb > 0) {
            uint64_t availableKb = memAvailableKb > 0 ? memAvailableKb : memFreeKb;
            if (availableKb > memTotalKb) {
                availableKb = memTotalKb;
            }
            systemMetricsState.memTotalKb = memTotalKb;
            systemMetricsState.memUsedKb = memTotalKb - availableKb;
            systemMetricsState.ramUsage = clampFloat((float)systemMetricsState.memUsedKb / (float)memTotalKb, 0.0f, 1.0f);
        }
    }
}

static inline void updateScrollArea(ScrollAreaState &state, TouchState *touch, int x, int y, int w, int h, int maxOffset) {
    const bool touchDown = touch && touch->is_pressed;
    const int touchX = touch ? touch->x : state.lastX;
    const int touchY = touch ? touch->y : state.lastY;

    if (touchDown) {
        state.lastX = touchX;
        state.lastY = touchY;
    }

    if (touchDown && !state.wasTouchDown && pointInRect(touchX, touchY, x, y, w, h)) {
        state.tracking = true;
        state.dragged = false;
        state.startX = touchX;
        state.startY = touchY;
        state.startOffset = state.offset;
    } else if (touchDown && state.tracking) {
        int dy = touchY - state.startY;
        if (dy > 4 || dy < -4) {
            state.dragged = true;
        }
        state.offset = clampInt(state.startOffset - dy, 0, maxOffset);
    } else if (!touchDown && state.wasTouchDown) {
        state.tracking = false;
        state.dragged = false;
    }

    state.wasTouchDown = touchDown;
    state.offset = clampInt(state.offset, 0, maxOffset);
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

    drawImage(x, y, 0, 0, (const unsigned char *)boot_logo_data);
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

    const bool touchDown = touchState && touchState->is_pressed;

    if (uiSleepEnabled) {
        if (!uiSleepFrameDrawn) {
            beginFrame();
            drawRect(0, 0, width, height, 0, 0, 0);
            present();
            uiSleepFrameDrawn = true;
        }

        if (touchDown && !uiSleepLastTouchDown) {
            uiSleepEnabled = false;
            uiSleepFrameDrawn = false;
        }

        uiSleepLastTouchDown = touchDown;
        return;
    }

    uiSleepLastTouchDown = touchDown;

    refreshSystemMetrics();
    beginFrame();

    drawRect(0, 0, width, height, 15, 20, 30);

    const int padding = 10;
    const int panelWidth = (int)width - (2 * padding);
    const int logoHeight = 60;
    const int metricsHeight = 74;
    const int buttonHeight = 45;
    const int buttonCount = 3;
    const int gap = 10;
    const int buttonsY = (int)height - padding - buttonHeight;
    const int metricsY = padding + logoHeight + gap;
    const int listY = metricsY + metricsHeight + gap;
    const int listHeight = buttonsY - gap - listY;
    const int headerHeight = 18;

    AudioDeviceInfo deviceInfos[kMaxUiDevices];
    size_t deviceCount = 0;
    ConfigData liveCfg = {};
    if (app && app->audio) {
        deviceCount = app->audio->copyDeviceInfos(deviceInfos, kMaxUiDevices);
        sortDeviceInfos(deviceInfos, deviceCount);
    }
    if (app && app->config) {
        liveCfg = app->config->readConfigFile();
    }

    int contentHeight = headerHeight + 8;
    for (size_t i = 0; i < deviceCount; ++i) {
        contentHeight += deviceRowHeight(deviceInfos[i]);
    }
    int maxScroll = 0;
    if (contentHeight > listHeight) {
        maxScroll = contentHeight - listHeight;
    }
    updateScrollArea(deviceListScrollState, touchState, padding, listY, panelWidth, listHeight, maxScroll);

    // Draw scaled boot logo in header area
    drawRect(padding, padding, panelWidth, logoHeight, 15, 20, 30);
    drawRectBorder(this, padding, padding, panelWidth, logoHeight, 60, 80, 110, 1);
    
    if (boot_logo_data) {
        int img_w = 0;
        int img_h = 0;
        const char *pixel_data = (const char *)boot_logo_data;
        if (sscanf(pixel_data, "P6\n%d %d\n255\n", &img_w, &img_h) == 2 && img_h > 0) {
            // Scale to fit height perfectly with small margins
            float scale = (float)(logoHeight - 4) / (float)img_h;
            int scaled_w = (int)((float)img_w * scale);
            int scaled_h = (int)((float)img_h * scale);
            
            // Ensure it fits within panel width
            if (scaled_w > panelWidth - 8) {
                scale = (float)(panelWidth - 8) / (float)img_w;
                scaled_w = (int)((float)img_w * scale);
                scaled_h = (int)((float)img_h * scale);
            }
            
            int logo_x = padding + (panelWidth - scaled_w) / 2;
            int logo_y = padding + (logoHeight - scaled_h) / 2;
            drawImage(logo_x, logo_y, scaled_w, scaled_h, (const unsigned char *)boot_logo_data);
        }
    }

    drawRect(padding, metricsY, panelWidth, metricsHeight, 25, 35, 50);
    drawRectBorder(this, padding, metricsY, panelWidth, metricsHeight, 80, 120, 160, 1);
    drawText(padding + 8, metricsY + 6, "system", 150, 188, 228, 1);

    const int metricBarX = padding + 52;
    const int labelWidth = 75;
    const int metricBarW = panelWidth - metricBarX - labelWidth - 12;
    const int cpuBarY = metricsY + 18;
    const int ramBarY = metricsY + 43;
    drawText(padding + 8, cpuBarY, "cpu", 200, 220, 255, 1);
    drawText(padding + 8, ramBarY, "ram", 200, 220, 255, 1);
    drawRect(metricBarX, cpuBarY, metricBarW, 12, 40, 50, 70);
    drawRect(metricBarX, ramBarY, metricBarW, 12, 40, 50, 70);
    drawRectBorder(this, metricBarX, cpuBarY, metricBarW, 12, 80, 100, 120, 1);
    drawRectBorder(this, metricBarX, ramBarY, metricBarW, 12, 80, 100, 120, 1);
    drawMeterFill(this, metricBarX, cpuBarY, metricBarW, 12, systemMetricsState.cpuUsage);
    drawMeterFill(this, metricBarX, ramBarY, metricBarW, 12, systemMetricsState.ramUsage);

    char cpuLabel[32];
    char ramLabel[48];
    snprintf(cpuLabel, sizeof(cpuLabel), "%3d%%", (int)(systemMetricsState.cpuUsage * 100.0f + 0.5f));
    snprintf(ramLabel,
             sizeof(ramLabel),
             "%3d%% %lluM",
             (int)(systemMetricsState.ramUsage * 100.0f + 0.5f),
             (unsigned long long)(systemMetricsState.memUsedKb / 1024ULL));
    
    // Right-align labels after the bar with a small gap
    const int labelX = metricBarX + metricBarW + 8;
    drawText(labelX, cpuBarY, cpuLabel, 210, 225, 240, 1);
    drawText(labelX, ramBarY, ramLabel, 210, 225, 240, 1);

    drawRect(padding, listY, panelWidth, listHeight, 24, 31, 44);
    drawRectBorder(this, padding, listY, panelWidth, listHeight, 80, 120, 160, 1);
    drawText(padding + 8, listY + 6, "devices", 150, 188, 228, 1);

    const int listInnerX = padding + 8;
    const int listInnerY = listY + headerHeight;
    const int listInnerW = panelWidth - 16;
    const int meterW = 132;
    const int rowTextW = listInnerW - meterW - 16;
    const int maxNameChars = rowTextW / 8;
    const uint64_t nowMs = uiMonotonicMs();

    if (deviceCount == 0) {
        drawText(listInnerX, listInnerY + 10, "No audio devices found.", 190, 205, 220, 1);
    }

    int rowCursorY = listInnerY - deviceListScrollState.offset;
    for (size_t i = 0; i < deviceCount; ++i) {
        int rowHeight = deviceRowHeight(deviceInfos[i]);
        int rowY = rowCursorY;
        rowCursorY += rowHeight;
        if (rowY < listInnerY || (rowY + rowHeight - 2) > (listY + listHeight - 2)) {
            continue;
        }

        unsigned char rowShade = (i % 2 == 0) ? 34 : 29;
        drawRect(listInnerX, rowY, listInnerW, rowHeight - 2, rowShade, rowShade + 8, rowShade + 20);
        drawRectBorder(this, listInnerX, rowY, listInnerW, rowHeight - 2, 60, 86, 112, 1);

        char nameLabel[64];
        char detailLabel[64];
        trimLabelToWidth(deviceInfos[i].displayName, nameLabel, sizeof(nameLabel), maxNameChars);
        snprintf(detailLabel,
                 sizeof(detailLabel),
                 "c%u d%u %s p%u c%u",
                 (unsigned)deviceInfos[i].cardIndex,
                 (unsigned)deviceInfos[i].deviceIndex,
                 deviceInfos[i].isUsb ? "usb" : "local",
                 (unsigned)channelCountForUi(deviceInfos[i].playbackChannels, deviceInfos[i].hasPlayback),
                 (unsigned)channelCountForUi(deviceInfos[i].captureChannels, deviceInfos[i].hasCapture));

        drawText(listInnerX + 6, rowY + 4, nameLabel, 220, 232, 240, 1);
        drawText(listInnerX + 6, rowY + 16, detailLabel, 150, 170, 190, 1);

        const int meterX = listInnerX + listInnerW - meterW - 8;
        const int meterY = rowY + 4;
        const int meterH = rowHeight - 8;
        drawRect(meterX, meterY, meterW, meterH, 40, 50, 70);
        drawRectBorder(this, meterX, meterY, meterW, meterH, 72, 92, 118, 1);

        int playbackCount = channelCountForUi(deviceInfos[i].playbackChannels, deviceInfos[i].hasPlayback);
        int captureCount = channelCountForUi(deviceInfos[i].captureChannels, deviceInfos[i].hasCapture);
        int sectionGap = 6;
        int sectionCount = 0;
        if (playbackCount > 0) {
            sectionCount++;
        }
        if (captureCount > 0) {
            sectionCount++;
        }
        int sectionW = meterW - 10;
        if (sectionCount == 2) {
            sectionW = (meterW - 16 - sectionGap) / 2;
        }
        int sectionX = meterX + 5;
        if (playbackCount > 0) {
            drawChannelMeterGroup(this,
                                  sectionX,
                                  meterY + 3,
                                  sectionW,
                                  playbackCount,
                                  captureCount > 0 ? "out" : "play",
                                  deviceInfos[i].handle,
                                  0,
                                  nowMs,
                                  230,
                                  148,
                                  58);
            sectionX += sectionW + sectionGap;
        }
        if (captureCount > 0) {
            drawChannelMeterGroup(this,
                                  sectionX,
                                  meterY + 3,
                                  sectionW,
                                  captureCount,
                                  playbackCount > 0 ? "in" : "cap",
                                  deviceInfos[i].handle,
                                  1,
                                  nowMs,
                                  66,
                                  190,
                                  180);
        }
    }

    if (maxScroll > 0 && listHeight > 0) {
        const int trackX = padding + panelWidth - 6;
        const int trackY = listY + headerHeight + 2;
        const int trackH = listHeight - headerHeight - 4;
        int thumbH = (trackH * listHeight) / contentHeight;
        if (thumbH < 18) {
            thumbH = 18;
        }
        int thumbTravel = trackH - thumbH;
        int thumbY = trackY;
        if (thumbTravel > 0) {
            thumbY += (thumbTravel * deviceListScrollState.offset) / maxScroll;
        }
        drawRect(trackX, trackY, 3, trackH, 42, 52, 68);
        drawRect(trackX, thumbY, 3, thumbH, 112, 152, 194);
    }

    for (int i = 0; i < buttonCount; i++) {
        int buttonW = (panelWidth - ((buttonCount - 1) * padding)) / buttonCount;
        int btnX = padding + i * (buttonW + padding);
        const char *btnLabel = nullptr;
        
        switch (i) {
            case 0: btnLabel = "Sleep"; break;
            case 1: btnLabel = "Reboot"; break;
            case 2: btnLabel = "Shutdown"; break;
        }
        
        int pressed = imuiButton(this, i, btnX, buttonsY, buttonW, buttonHeight, btnLabel, touchState);
        if (pressed) {
            if (i == 0) {
                uiSleepEnabled = true;
                uiSleepFrameDrawn = false;
            } else if (i == 1) {
                requestSystemReboot();
            } else if (i == 2) {
                requestSystemPoweroff();
            }
        }
    }

    present();
}