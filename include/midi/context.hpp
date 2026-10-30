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
    uint8_t lastChannel{0};
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
    bool lightsDimmed;
    uint8_t heldNote;
    int holdActive;
    uint8_t samplerModeActive;
    uint8_t samplerSelectedValid;
    char samplerSelectedSfx[MIDI_SFX_PATH_MAX];
    MidiUsbOutput usb;
    MidiStream usbStream;
    uint64_t nextUsbConfigMs{0};
    uint8_t passthroughMode{0};
    bool keyboardEnabled{false};
    MidiChord compiledMacros[MIDI_CHORDS_MAX]{};
    unsigned compiledMacroCount{0};
    MidiScriptPlan macroPlan{};
    MidiScriptState macroStates[MIDI_CHORDS_MAX]{};
    char macroErrors[MIDI_CHORDS_MAX][192]{};
    bool macroValid[MIDI_CHORDS_MAX]{};
    bool macroUsesKeyboard[MIDI_CHORDS_MAX]{};
    bool macroPressed[MIDI_CHORDS_MAX]{};
    int macroLight[MIDI_CHORDS_MAX]{};
    struct MacroTrigger {
        uint8_t index;
        MidiScriptEvent event;
    } macroQueue[MIDI_CHORDS_MAX]{};
    unsigned macroQueueCount{0};
    int activeMacro{-1};
    unsigned macroStep{0};
    uint64_t macroWakeMs{0};
    uint64_t macroDeadlineMs{0};
    bool macroWaitingTap{false};
    unsigned macroTextIndex{0};
    bool macroTextPending{false};
    bool macroWaitingController{false};
    uint64_t nextMacroEvalMs{0};
    uint64_t macroUsbGeneration{0};
    uint8_t controllerPending[4096]{};
    size_t controllerPendingSize{0};
    bool controllerOutputFailed{false};
    bool macroMidiNotes[16][128]{};
    bool macroMidiSustain[16]{};

    MidiContext(Audiox *context);

    void poll();
    void disconnect();

    // Send 3-byte raw MIDI message to the connected device.
    void sendRawMidi(uint8_t b0, uint8_t b1, uint8_t b2);

    // Send note-on (or note-off when velocity=0) for LED lighting.
    void sendLightNote(uint8_t note, uint8_t channel, uint8_t velocity);

    // Refresh all mapped buttons to their "mapped" (idle) lighting state.
    void refreshAllLighting();

    void setLightsDimmed(bool dimmed);
    void refreshLightingFromState();
    int reloadSoundboardMappings();
    void syncMacros();
    void stopMacros();
    bool triggerMacro(unsigned index, uint8_t velocity = 127, bool fromWeb = false);
    void pollMacros(uint64_t now);
    bool queueControllerMidi(const uint8_t *bytes, size_t count);
    bool flushControllerMidi();
    void releaseMacroMidi();
    void failMacro(const char *error);
};