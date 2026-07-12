#pragma once

#include "../context.hpp"

#define TOUCH_MAX_POINTS 10

struct TouchEvent {
    int x;
    int y;
    bool pressed;
};

struct TouchContext {
    Audiox *app;
    int fd;
    int supports_mt_slots;
    int max_slots;
    int current_slot;
    int slot_active[TOUCH_MAX_POINTS];
    int slot_x[TOUCH_MAX_POINTS];
    int slot_y[TOUCH_MAX_POINTS];
    int min_x;
    int max_x;
    int min_y;
    int max_y;
    int has_range;

    TouchContext(Audiox *context);

    void poll(TouchEvent *event);
};