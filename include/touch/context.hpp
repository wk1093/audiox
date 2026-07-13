#pragma once

#include "../context.hpp"

#define TOUCH_MAX_POINTS 10

struct TouchState {
    bool is_pressed{false};
    int x{0};
    int y{0};
};

struct TouchContext {
    Audiox *app;
    int fd{-1};
    int supports_mt_slots{0};
    int max_slots{TOUCH_MAX_POINTS};
    int current_slot{0};
    int slot_active[TOUCH_MAX_POINTS]{0};
    int slot_x[TOUCH_MAX_POINTS]{0};
    int slot_y[TOUCH_MAX_POINTS]{0};
    int min_x{0};
    int max_x{0};
    int min_y{0};
    int max_y{0};
    int has_range{0};
    TouchState state{};

    TouchContext(Audiox *context);

    void poll();

    inline bool isReady() const {
        return fd >= 0;
    }

    void checkForDevice();
};