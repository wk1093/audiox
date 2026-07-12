#include "context.hpp"

#include <stdio.h>

Audiox::Audiox() : audio(nullptr), midi(nullptr), touch(nullptr), fb(nullptr), config(nullptr), http(nullptr), ready(false) {
}

void Audiox::setReady() {
    if (ready) {
        return;
    }
    ready = true;
    printf("[INIT] Audiox context is ready\n");
}

void Audiox::eventLoop() {
    
}