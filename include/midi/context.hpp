#pragma once

#include <cstdint>

#include "../context.hpp"
#include "config/context.hpp"

struct MidiContext {
    Audiox *app;
    int fd;
    int connected;
    char devPath[256];
    uint8_t runningStatus;
    uint8_t dataBuf[2];
    int dataHave;
    int dataNeed;
    uint64_t nextProbeMs;
    uint64_t nextConfigReloadMs;
    uint32_t lastNoteSeq;
    uint8_t lastNote;
    uint8_t lastVelocity;
    ConfigData cachedConfig;

    MidiContext(Audiox *context);

    void poll();
    void disconnect();
};