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

    // Lighting state
    MidiMapData cachedMidiMap;
    uint64_t nextMidiMapReloadMs;
    uint8_t  currentLitNote;  // note currently in "playing" state; 255 = none
    int      sfxWasPlaying;   // previous sfxIsPlaying value
    uint32_t cachedTriggerSeq;
    uint8_t heldNote;
    int holdActive;

    MidiContext(Audiox *context);

    void poll();
    void disconnect();

    // Send 3-byte raw MIDI message to the connected device.
    void sendRawMidi(uint8_t b0, uint8_t b1, uint8_t b2);

    // Send note-on (or note-off when velocity=0) for LED lighting.
    void sendLightNote(uint8_t note, uint8_t channel, uint8_t velocity);

    // Refresh all mapped buttons to their "mapped" (idle) lighting state.
    void refreshAllLighting();

    // Called when a sound starts/stops playing so lighting can be updated.
    void notifySoundStarted(const char *sfxBasename);
    void notifySoundStopped();
};