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
    uint32_t lastNoteSeq;
    uint8_t lastNote;
    uint8_t lastVelocity;
    uint32_t lastCcSeq;
    uint8_t lastCc;
    uint8_t lastCcValue;

    // Lighting state
    MidiMapData cachedMidiMap;
    uint64_t nextMidiMapReloadMs;
    uint32_t cachedPlayingSeq;
    uint32_t cachedPlayingCount;
    char cachedPlayingSfx[MIDI_MAPPINGS_MAX][MIDI_SFX_PATH_MAX];
    uint8_t ledStateByNote[128];
    uint8_t heldNote;
    int holdActive;
    uint8_t samplerModeActive;
    uint8_t samplerSelectedValid;
    char samplerSelectedSfx[MIDI_SFX_PATH_MAX];

    MidiContext(Audiox *context);

    void poll();
    void disconnect();

    // Send 3-byte raw MIDI message to the connected device.
    void sendRawMidi(uint8_t b0, uint8_t b1, uint8_t b2);

    // Send note-on (or note-off when velocity=0) for LED lighting.
    void sendLightNote(uint8_t note, uint8_t channel, uint8_t velocity);

    // Refresh all mapped buttons to their "mapped" (idle) lighting state.
    void refreshAllLighting();

    void refreshLightingFromState();
};