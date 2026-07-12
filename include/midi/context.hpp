#pragma once

#include "../context.hpp"

struct MidiContext {
    Audiox *app;

    MidiContext(Audiox *context);

    void poll();
};