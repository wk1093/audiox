#include "touch/context.hpp"
#include "framebuffer/context.hpp"

#include "defs.hpp"

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <time.h>

static inline int touchTestBit(const unsigned long *bits, unsigned int bit) {
    return (bits[bit / (8U * sizeof(unsigned long))] >> (bit % (8U * sizeof(unsigned long)))) & 1UL;
}

static inline int touchQueryBits(int fd, unsigned int ev, unsigned long *bits, size_t bits_len) {
    memset(bits, 0, bits_len);
    return ioctl(fd, EVIOCGBIT(ev, bits_len), bits);
}

static inline void touchCopyString(char *dst, size_t dst_size, const char *src) {
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

static inline int touchIsCandidateDevice(int fd, int *score_out) {
    unsigned long ev_bits[(EV_MAX / (8 * sizeof(unsigned long))) + 2];
    unsigned long abs_bits[(ABS_MAX / (8 * sizeof(unsigned long))) + 2];
    unsigned long key_bits[(KEY_MAX / (8 * sizeof(unsigned long))) + 2];
    int score = 0;

    if (touchQueryBits(fd, 0, ev_bits, sizeof(ev_bits)) < 0) {
        return 0;
    }
    if (!touchTestBit(ev_bits, EV_ABS)) {
        return 0;
    }
    if (touchQueryBits(fd, EV_ABS, abs_bits, sizeof(abs_bits)) < 0) {
        return 0;
    }

    int has_abs_xy = touchTestBit(abs_bits, ABS_X) && touchTestBit(abs_bits, ABS_Y);
    int has_mt_xy = touchTestBit(abs_bits, ABS_MT_POSITION_X) && touchTestBit(abs_bits, ABS_MT_POSITION_Y);
    int has_mt_slot = touchTestBit(abs_bits, ABS_MT_SLOT);

    if (!has_abs_xy && !has_mt_xy) {
        return 0;
    }

    // Only screens: a direct-input device (touchscreen, pen display) or one that
    // reports touches. Touchpads, tablets, joysticks and gamepads also have
    // absolute axes but do not map onto the display.
    unsigned long prop_bits[(INPUT_PROP_MAX / (8 * sizeof(unsigned long))) + 2];
    memset(prop_bits, 0, sizeof(prop_bits));
    int have_props = ioctl(fd, EVIOCGPROP(sizeof(prop_bits)), prop_bits) >= 0;
    int direct = have_props && touchTestBit(prop_bits, INPUT_PROP_DIRECT);
    int pointer = have_props && touchTestBit(prop_bits, INPUT_PROP_POINTER);
    int has_keys = touchQueryBits(fd, EV_KEY, key_bits, sizeof(key_bits)) >= 0;
    int btn_touch = has_keys && touchTestBit(key_bits, BTN_TOUCH);
    if (has_keys && (touchTestBit(key_bits, BTN_JOYSTICK) || touchTestBit(key_bits, BTN_GAMEPAD))) {
        return 0;
    }
    if (!direct && (pointer || !btn_touch)) {
        return 0;
    }

    if (direct) {
        score += 8;
    }
    if (btn_touch) {
        score += 3;
    }
    if (has_mt_xy) {
        score += 4;
    }
    if (has_abs_xy) {
        score += 2;
    }
    if (has_mt_slot) {
        score += 1;
    }

    if (score_out) {
        *score_out = score;
    }
    return score > 0;
}



// similar to the framebuffer context, we want to be able to poll if the device isn't found
static inline int getTouchDevice(TouchContext *touch) {
    if (!touch) {
        return -1;
    }

    int best_fd = -1;
    int best_score = 0;
    for (int i = 0; i < 32; ++i) {
        char path[32];
        int path_len = snprintf(path, sizeof(path), "/dev/input/event%d", i);
        if (path_len < 0 || (size_t)path_len >= sizeof(path)) {
            continue;
        }

        int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            continue;
        }

        int score = 0;
        if (touchIsCandidateDevice(fd, &score) && score > best_score) {
            if (best_fd >= 0) close(best_fd);
            best_fd = fd;
            best_score = score;
            continue;
        }

        close(fd);
    }

    if (best_fd < 0) {
        return RET_ERR;
    }
    touch->fd = best_fd;
    return RET_OK;
}

static inline int touchProbeAbsAxis(int fd, unsigned int axis, int *min_out, int *max_out) {
    struct input_absinfo abs_info;
    if (ioctl(fd, EVIOCGABS(axis), &abs_info) < 0) {
        return -1;
    }
    if (min_out) {
        *min_out = abs_info.minimum;
    }
    if (max_out) {
        *max_out = abs_info.maximum;
    }
    return 0;
}

// Maps a raw axis value onto [0, size) of the display; touch digitizers report
// their own resolution (e.g. 0..4095 on USB touchscreens).
static inline int touchScale(int value, int min, int max, uint32_t size) {
    if (size == 0 || max <= min) {
        return value;
    }
    long long scaled = (long long)(value - min) * (long long)(size - 1) / (long long)(max - min);
    if (scaled < 0) scaled = 0;
    if (scaled > (long long)size - 1) scaled = (long long)size - 1;
    return (int)scaled;
}

TouchContext::TouchContext(Audiox *context) : app(context) {
    if (app) app->touch = this;
}

TouchContext::~TouchContext() {
    suspend();
    if (app && app->touch == this) app->touch = nullptr;
}

void TouchContext::suspend() {
    if (fd >= 0) {
        close(fd);
        if (app && app->fb) app->fb->resetUi();
    }
    fd = -1;
    state = {};
    memset(slot_active, 0, sizeof(slot_active));
    memset(slot_x, 0, sizeof(slot_x));
    memset(slot_y, 0, sizeof(slot_y));
    supports_mt_slots = current_slot = has_range = 0;
    max_slots = 1;
    min_x = max_x = min_y = max_y = 0;
}

void TouchContext::poll() {
    if (!app || !isReady()) {
        return;
    }

    struct input_event ev;
    ssize_t n;

    while ((n = read(fd, &ev, sizeof(ev))) == (ssize_t)sizeof(ev)) {
        if (ev.type == EV_ABS) {
            if (ev.code == ABS_MT_SLOT && supports_mt_slots) {
                if (ev.value >= 0 && ev.value < max_slots) {
                    current_slot = ev.value;
                }
            } else if (ev.code == ABS_X) {
                slot_x[0] = ev.value;
            } else if (ev.code == ABS_Y) {
                slot_y[0] = ev.value;
            } else if (ev.code == ABS_MT_POSITION_X) {
                slot_x[current_slot] = ev.value;
            } else if (ev.code == ABS_MT_POSITION_Y) {
                slot_y[current_slot] = ev.value;
            } else if (ev.code == ABS_MT_TRACKING_ID) {
                slot_active[current_slot] = (ev.value >= 0) ? 1 : 0;
            } else if (ev.code == ABS_PRESSURE && !supports_mt_slots) {
                slot_active[0] = (ev.value > 0) ? 1 : 0;
            } else if (ev.code == ABS_MT_PRESSURE && supports_mt_slots) {
                if (ev.value == 0 && slot_active[current_slot]) {
                    slot_active[current_slot] = 0;
                }
            }
        }

        if (ev.type == EV_KEY && ev.code == BTN_TOUCH) {
            if (supports_mt_slots) {
                if (ev.value == 0) {
                    for (int i = 0; i < max_slots; ++i) {
                        slot_active[i] = 0;
                    }
                }
            } else {
                slot_active[0] = (ev.value > 0) ? 1 : 0;
            }
        }
    }

    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
        printf("[TOUCH] [WARN] Touch device disconnected: %s\n", n == 0 ? "end of input" : strerror(errno));
        suspend();
        return;
    }

    // update state from the first active slot
    state.is_pressed = false;
    for (int i = 0; i < max_slots; ++i) {
        if (slot_active[i]) {
            state.x = slot_x[i];
            state.y = slot_y[i];
            if (has_range && app->fb && app->fb->isReady()) {
                state.x = touchScale(slot_x[i], min_x, max_x, app->fb->width);
                state.y = touchScale(slot_y[i], min_y, max_y, app->fb->height);
            }
            state.is_pressed = true;
            break;
        }
    }
}

void TouchContext::checkForDevice() {
    if (!app) {
        return;
    }
    if (isReady()) {
        return;
    }
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint64_t now = (uint64_t)ts.tv_sec * 1000U + (uint64_t)ts.tv_nsec / 1000000U;
    if (now < nextProbeMs) return;
    nextProbeMs = now + AUDIOX_DEVICE_POLL_INTERVAL_MS;

    int ret = getTouchDevice(this);
    if (ret == RET_OK) {
        printf("[TOUCH] Touch input device found during runtime: fd=%d\n", fd);
        // Positions come from the MT axes when the device has them.
        int ok_x = (touchProbeAbsAxis(fd, ABS_MT_POSITION_X, &min_x, &max_x) == 0) ||
                   (touchProbeAbsAxis(fd, ABS_X, &min_x, &max_x) == 0);
        int ok_y = (touchProbeAbsAxis(fd, ABS_MT_POSITION_Y, &min_y, &max_y) == 0) ||
                   (touchProbeAbsAxis(fd, ABS_Y, &min_y, &max_y) == 0);
        has_range = ok_x && ok_y && (max_x > min_x) && (max_y > min_y);
        printf("[TOUCH] Touch input device range: x=[%d, %d], y=[%d, %d]\n", min_x, max_x, min_y, max_y);

        struct input_absinfo slot_info;
        if (ioctl(fd, EVIOCGABS(ABS_MT_SLOT), &slot_info) == 0 && slot_info.maximum >= 0) {
            int slots = slot_info.maximum + 1;
            if (slots > TOUCH_MAX_POINTS) slots = TOUCH_MAX_POINTS;
            supports_mt_slots = 1;
            max_slots = slots;
        } else {
            supports_mt_slots = 0;
            max_slots = 1;
        }
        current_slot = 0;
    }

    
}