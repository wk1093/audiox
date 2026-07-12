#pragma once

#include "defs.hpp"
#include "../context.hpp"

struct HttpServer {
    Audiox *app;

    HttpServer(Audiox *context);

    int startSocket() WARN_UNUSED;

    void poll();
};