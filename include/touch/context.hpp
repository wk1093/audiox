#pragma once

#include "../context.hpp"

struct TouchEvent {
    int x;
    int y;
    bool pressed;
};

struct TouchContext {
    Audiox *app;
    int fd;
    int width;
    int height;

    TouchContext(Audiox *context);

    void poll(TouchEvent *event);
};