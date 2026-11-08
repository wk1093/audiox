#pragma once

#include "midi/usb.hpp"

struct MidiContext;

struct ScriptContext {
  MidiContext &midi;
  explicit ScriptContext(MidiContext &controller);
  ~ScriptContext();
  MidiUsbOutput usb;
  ScriptDefinition compiledMacros[SCRIPTS_MAX]{};
  unsigned compiledMacroCount{0};
  ScriptPlan macroPlan{};
  ScriptState macroStates[SCRIPTS_MAX]{};
  char macroErrors[SCRIPTS_MAX][192]{};
  bool macroValid[SCRIPTS_MAX]{};
  uint16_t macroEventMasks[SCRIPTS_MAX]{};
  bool bootHooksDispatched{false};
  uint64_t nextScriptTimerMs{0};
  uint64_t nextDeviceActivityMs{0};
  ScriptActivity deviceActivity[SCRIPTS_MAX]{};
  uint64_t scriptBluetoothHash{0};
  bool scriptBluetoothKnown{false};
  bool macroUsesKeyboard[SCRIPTS_MAX]{};
  bool macroPressed[SCRIPTS_MAX]{};
  int macroLight[SCRIPTS_MAX]{};
  struct MacroTrigger {
    uint8_t index;
    ScriptEvent event;
  } macroQueue[SCRIPTS_MAX]{};
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

  void syncMacros();
  void stopMacros();
  bool triggerMacro(unsigned index, uint8_t velocity = 127,
                    bool fromWeb = false);
  bool queueMacroEvent(unsigned index, ScriptEvent event);
  void dispatchMacroEvent(ScriptEvent event);
  void pollScriptLifecycle(uint64_t now, bool startupStable);
  void pollDeviceActivity(uint64_t now);
  void pollBluetoothMediaEvents();
  void pollMacros(uint64_t now);
  bool queueControllerMidi(const uint8_t *bytes, size_t count);
  bool flushControllerMidi();
  void releaseMacroMidi();
  void failMacro(const char *error);
};
