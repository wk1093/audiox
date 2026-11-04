#pragma once

#include <cstddef>

struct AudioContext;
struct MidiContext;
struct ScriptContext;
struct TouchContext;
struct FramebufferContext;
struct ConfigStore;
struct HttpServer;
struct BluetoothContext;

struct Audiox {
    AudioContext *audio;
    MidiContext *midi;
    ScriptContext *scripts{nullptr};
    TouchContext *touch;
    FramebufferContext *fb;
    ConfigStore *config;
    HttpServer *http;
    BluetoothContext *bluetooth;
    bool ready;

    Audiox();

    void setReady();
    void eventLoop();
};