#pragma once

#include "defs.hpp"
#include "../context.hpp"

struct AudioContext {
    Audiox *app;

    AudioContext(Audiox *context);

    int setupThreads() WARN_UNUSED;
    int triggerSfx(const char *sfxPath) WARN_UNUSED;
    void poll();
};