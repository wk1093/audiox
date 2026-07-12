#pragma once

#include <cstddef>

struct AudioContext;
struct MidiContext;
struct TouchContext;
struct FramebufferContext;
struct ConfigStore;
struct HttpServer;

struct Audiox {
    AudioContext *audio;
    MidiContext *midi;
    TouchContext *touch;
    FramebufferContext *fb;
    ConfigStore *config;
    HttpServer *http;
    bool ready;

    Audiox();

    void setReady();
    void eventLoop();
};