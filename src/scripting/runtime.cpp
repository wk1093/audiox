#include "audio/context.hpp"
#include "audio/activity.hpp"
#include "audio/processing_fx.hpp"
#include "init.hpp"
#include "midi/context.hpp"
#include "scripting/context.hpp"
#include "scripting/device.hpp"
#include "scripting/storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/reboot.h>
#include <time.h>
#include <unistd.h>

ScriptContext::ScriptContext(MidiContext &controller) : midi(controller) {
  if (midi.app)
    midi.app->scripts = this;
}

ScriptContext::~ScriptContext() {
  if (midi.app && midi.app->scripts == this)
    midi.app->scripts = nullptr;
}

void ScriptContext::stopMacros() {
  releaseMacroMidi();
  usb.cancelChords();
  activeMacro = -1;
  macroQueueCount = 0;
  macroWaitingTap = false;
  macroTextIndex = 0;
  macroTextPending = false;
  macroWaitingController = false;
  for (auto &state : macroStates)
    state = {};
  memset(macroPressed, 0, sizeof(macroPressed));
  nextDeviceActivityMs = 0;
  for (auto &light : macroLight)
    light = -1;
}

void ScriptContext::syncMacros() {
  unsigned count = std::min(midi.cachedMidiMap.chordCount, SCRIPTS_MAX);
  if (count == compiledMacroCount &&
      std::equal(compiledMacros, compiledMacros + count,
                 midi.cachedMidiMap.chords, scriptMacroEqual))
    return;
  stopMacros();
  for (auto &activity : deviceActivity) activity = {};
  compiledMacroCount = count;
  memcpy(compiledMacros, midi.cachedMidiMap.chords,
         count * sizeof(ScriptDefinition));
  for (unsigned i = 0; i < count; ++i) {
    const auto &macro = compiledMacros[i];
    macroErrors[i][0] = '\0';
    char error[192]{};
    macroEventMasks[i] = macro.eventMask;
    if (macro.script[0]) {
      macroValid[i] =
          macro.hookMode && scriptInspectHooks(macro.script, macroEventMasks[i],
                                               error, sizeof(error), &deviceActivity[i]);
    } else {
      macroValid[i] = false;
    }
    if (!macroValid[i]) {
      snprintf(macroErrors[i], sizeof(macroErrors[i]), "%s",
               error[0] ? error : "script requires callback conversion");
      printf("[SCRIPT] [WARN] Macro ch=%u note=%u disabled: %s\n",
             macro.channel, macro.note, macroErrors[i]);
    }
  }
  midi.refreshLightingFromState();
}

bool ScriptContext::triggerMacro(unsigned index, uint8_t velocity,
                                 bool fromWeb) {
  if (index >= compiledMacroCount)
    return false;
  const auto &macro = compiledMacros[index];
  ScriptEvent event{macro.channel, macro.note, velocity, true};
  event.fromWeb = fromWeb;
  if (macro.hookMode && fromWeb) {
    event.kind = ScriptEventKind::Run;
    if (!(macroEventMasks[index] & (1U << static_cast<unsigned>(event.kind)))) {
      printf("[SCRIPT] [WARN] Callback macro has no run hook\n");
      return false;
    }
  }
  return queueMacroEvent(index, event);
}

bool ScriptContext::queueMacroEvent(unsigned index, ScriptEvent event) {
  if (midi.passthroughOnly) {
    if (event.fromWeb) printf("[SCRIPT] [WARN] Scripts are paused in MIDI passthrough-only mode\n");
    return false;
  }
  if (index >= compiledMacroCount || !macroValid[index]) {
    printf("[SCRIPT] [WARN] Cannot queue unavailable macro %u\n", index);
    return false;
  }
  if (macroQueueCount == SCRIPTS_MAX) {
    printf("[SCRIPT] [WARN] Macro trigger queue full; dropping trigger\n");
    return false;
  }
  struct timespec monotonic{}, wall{};
  if (clock_gettime(CLOCK_MONOTONIC, &monotonic) != 0 ||
      clock_gettime(CLOCK_REALTIME, &wall) != 0) {
    printf("[SCRIPT] [WARN] Cannot capture macro clocks: %s\n",
           strerror(errno));
    return false;
  }
  event.monotonicMs = static_cast<uint64_t>(monotonic.tv_sec) * 1000 +
                      static_cast<uint64_t>(monotonic.tv_nsec) / 1000000;
  event.epochSeconds = static_cast<int64_t>(wall.tv_sec);
  macroQueue[macroQueueCount++] = {static_cast<uint8_t>(index), event};
  return true;
}

void ScriptContext::dispatchMacroEvent(ScriptEvent event) {
  if (midi.passthroughOnly) return;
  const unsigned kind = static_cast<unsigned>(event.kind);
  if (kind >= SCRIPT_EVENT_COUNT) {
    printf("[SCRIPT] [WARN] Invalid macro event kind %u\n", kind);
    return;
  }
  const bool midi =
      kind <= static_cast<unsigned>(ScriptEventKind::ControlChange);
  for (unsigned i = 0; i < compiledMacroCount; ++i) {
    const auto &macro = compiledMacros[i];
    if (!macroValid[i])
      continue;
    if (!(macroEventMasks[i] & (1U << kind)))
      continue;
    if (midi && (!macro.midiEnabled || macro.channel != event.channel ||
                 macro.note != event.note))
      continue;
    if (!queueMacroEvent(i, event))
      break;
  }
}

void ScriptContext::pollBluetoothMediaEvents() {
  auto *bluetooth = midi.app ? midi.app->bluetooth : nullptr;
  if (!bluetooth)
    return;
  BluetoothMediaEvent media{};
  for (unsigned i = 0; i < BluetoothContext::MEDIA_EVENTS_MAX &&
                       bluetooth->popMediaEvent(media);
       ++i) {
    const char *key = bt::mediaKeyName(media.key.operation);
    if (!key) {
      printf("[SCRIPT] [WARN] Invalid Bluetooth media key 0x%02x\n",
             media.key.operation);
      continue;
    }
    ScriptEvent event{};
    event.kind = ScriptEventKind::BluetoothMediaKey;
    event.pressed = media.key.pressed;
    bt::formatAddress(media.address, event.device, sizeof(event.device));
    snprintf(event.key, sizeof(event.key), "%s", key);
    dispatchMacroEvent(event);
  }
  const unsigned dropped = bluetooth->takeMediaEventsDropped();
  if (dropped)
    printf("[SCRIPT] [WARN] Dropped %u Bluetooth media Lua events: event queue "
           "full\n",
           dropped);
}

void ScriptContext::pollDeviceActivity(uint64_t now) {
  if (now < nextDeviceActivityMs) return;
  nextDeviceActivityMs = now + 100;
  auto *audio = midi.app ? midi.app->audio : nullptr;
  for (unsigned i = 0; i < compiledMacroCount; ++i) {
    if (!macroValid[i]) continue;
    for (unsigned d = 0; d < deviceActivity[i].count; ++d) {
      auto &device = deviceActivity[i].devices[d];
      bool active = false;
      if (audio) {
        std::lock_guard<std::mutex> lock(audio->routingGraphMutex);
        const auto &graph = audio->routingGraphPublished;
        for (unsigned n = 0; n < graph.thingCount; ++n) {
          if (strcmp(graph.things[n].id, device.id)) continue;
          const unsigned channels = std::min<unsigned>(16, graph.things[n].outputs ?
              graph.things[n].outputs : graph.things[n].inputs);
          for (unsigned ch = 0; ch < channels; ++ch)
            active |= !(audio->nodeChannelLevels[n][ch].load(std::memory_order_relaxed) <= audiox::kAudioActivityNoiseFloor);
          break;
        }
      }
      if (!active && !device.active) continue;
      ScriptEvent event{};
      event.kind = ScriptEventKind::DeviceActive;
      event.active = active;
      event.pressed = active;
      snprintf(event.device, sizeof(event.device), "%s", device.id);
      bool queued = false;
      for (unsigned j = 0; j < macroQueueCount; ++j) {
        auto &pending = macroQueue[j];
        if (pending.index != i || pending.event.kind != event.kind ||
            strcmp(pending.event.device, device.id)) continue;
        pending.event.active = active;
        pending.event.pressed = active;
        queued = true;
        break;
      }
      if (queued || queueMacroEvent(i, event)) device.active = active;
    }
  }
}

void ScriptContext::pollScriptLifecycle(uint64_t now, bool startupStable) {
  if (midi.passthroughOnly) return;
  if (!startupStable)
    return;
  ScriptBluetoothSnapshot bluetooth{};
  if (scriptBluetoothSnapshot(midi.app, bluetooth)) {
    const uint64_t hash = ::scriptBluetoothHash(bluetooth);
    if (scriptBluetoothKnown && hash != scriptBluetoothHash) {
      ScriptEvent event{};
      event.kind = ScriptEventKind::BluetoothChange;
      dispatchMacroEvent(event);
    }
    scriptBluetoothHash = hash;
    scriptBluetoothKnown = true;
  }
  if (!bootHooksDispatched) {
    unsigned subscribers = 0;
    for (unsigned i = 0; i < compiledMacroCount; ++i)
      if (macroValid[i] && (macroEventMasks[i] & (1U << 8)))
        ++subscribers;
    if (subscribers > SCRIPTS_MAX - macroQueueCount)
      return;
    ScriptEvent event{};
    event.kind = ScriptEventKind::Boot;
    dispatchMacroEvent(event);
    bootHooksDispatched = true;
    nextScriptTimerMs = now + 1000;
  }
  pollDeviceActivity(now);
  if (now < nextScriptTimerMs)
    return;
  nextScriptTimerMs = now + 1000;
  for (unsigned i = 0; i < compiledMacroCount; ++i) {
    if (!macroValid[i] || !(macroEventMasks[i] & (1U << 9)))
      continue;
    bool pending = false;
    for (unsigned j = 0; j < macroQueueCount; ++j)
      if (macroQueue[j].index == i &&
          macroQueue[j].event.kind == ScriptEventKind::Timer)
        pending = true;
    if (pending)
      continue;
    ScriptEvent event{};
    event.kind = ScriptEventKind::Timer;
    if (!queueMacroEvent(i, event))
      break;
  }
}

void ScriptContext::failMacro(const char *error) {
  if (activeMacro >= 0) {
    snprintf(macroErrors[activeMacro], sizeof(macroErrors[activeMacro]), "%s",
             error);
    printf("[SCRIPT] [WARN] Macro ch=%u note=%u failed: %s\n",
           compiledMacros[activeMacro].channel,
           compiledMacros[activeMacro].note, error);
  }
  stopMacros();
  midi.refreshLightingFromState();
}

void ScriptContext::pollMacros(uint64_t now) {
  if (!flushControllerMidi() && macroWaitingController && activeMacro >= 0) {
    failMacro("controller MIDI output failed");
    return;
  }
  if (midi.passthroughOnly) return;
  if (macroUsbGeneration != usb.sessionGeneration) {
    macroUsbGeneration = usb.sessionGeneration;
    if (activeMacro >= 0 && macroUsesKeyboard[activeMacro]) {
      failMacro("USB keyboard session ended");
      return;
    }
    for (unsigned i = 0; i < compiledMacroCount; ++i)
      if (macroUsesKeyboard[i])
        macroStates[i] = {};
  }
  if (activeMacro < 0) {
    if (!macroQueueCount || now < nextMacroEvalMs || usb.keyDown ||
        usb.chordCount)
      return;
    const auto trigger = macroQueue[0];
    activeMacro = trigger.index;
    --macroQueueCount;
    memmove(macroQueue, macroQueue + 1,
            macroQueueCount * sizeof(macroQueue[0]));
    nextMacroEvalMs = now + 10;
    const auto &macro = compiledMacros[activeMacro];
    macroPlan = {};
    macroErrors[activeMacro][0] = '\0';
    ScriptState nextState = macroStates[activeMacro];
    ScriptMeters meters{};
    if (midi.app && midi.app->audio) {
        auto *audio = midi.app->audio;
        std::lock_guard<std::mutex> lock(audio->routingGraphMutex);
        const auto &graph = audio->routingGraphPublished;
        static_assert(AUDIO_GRAPH_MAX_THINGS <= 64);
        meters.count = graph.thingCount;
        for (unsigned i = 0; i < meters.count; ++i) {
            auto &node = meters.nodes[i];
            snprintf(node.id, sizeof(node.id), "%s", graph.things[i].id);
            node.channels = std::min<unsigned>(16, graph.things[i].outputs ?
                                              graph.things[i].outputs : graph.things[i].inputs);
            for (unsigned ch = 0; ch < node.channels; ++ch)
                node.levels[ch] = audio->nodeChannelLevels[i][ch].load(std::memory_order_relaxed);
        }
    }
    {
      if (!scriptEvaluate(
              macro.script, trigger.event, nextState, macroPlan,
              macroErrors[activeMacro], sizeof(macroErrors[activeMacro]),
              midi.app ? midi.app->config : nullptr, macro.configWriteAllowed,
              macro.hookMode, macroEventMasks[activeMacro], macro.id,
              midi.app && midi.app->audio ? &meters : nullptr)) {
        printf("[SCRIPT] [WARN] Macro ch=%u note=%u failed: %s\n",
               macro.channel, macro.note, macroErrors[activeMacro]);
        activeMacro = -1;
        return;
      }
    }
    bool needsKeyboard = false, needsController = false, needsMedia = false,
         needsStandard = false;
    for (unsigned i = 0; i < macroPlan.count; ++i) {
      auto kind = macroPlan.actions[i].kind;
      if (kind == ScriptKind::Light && !macro.midiEnabled) {
        snprintf(macroErrors[activeMacro], sizeof(macroErrors[activeMacro]),
                 "light actions require a MIDI binding");
        printf("[SCRIPT] [WARN] Macro %s failed: %s\n", macro.name,
               macroErrors[activeMacro]);
        activeMacro = -1;
        return;
      }
      needsKeyboard |= kind == ScriptKind::Tap || kind == ScriptKind::Press ||
                       kind == ScriptKind::Release || kind == ScriptKind::Type;
      needsMedia |= (kind == ScriptKind::Tap || kind == ScriptKind::Press) &&
                    scriptMediaUsage(macroPlan.actions[i].keys);
      needsStandard |=
          kind == ScriptKind::Type ||
          ((kind == ScriptKind::Tap || kind == ScriptKind::Press) &&
           !scriptMediaUsage(macroPlan.actions[i].keys));
      needsController |= kind == ScriptKind::Light || kind == ScriptKind::Midi;
    }
    macroUsesKeyboard[activeMacro] = needsKeyboard;
    if ((needsKeyboard &&
         (!midi.keyboardEnabled || (usb.keyboardFd < 0 && usb.mediaFd < 0))) ||
        (needsStandard && usb.keyboardFd < 0) ||
        (needsMedia && usb.mediaFd < 0) ||
        (needsController && (!midi.connected || midi.fd < 0))) {
      snprintf(macroErrors[activeMacro], sizeof(macroErrors[activeMacro]), "%s",
               needsKeyboard && (!midi.keyboardEnabled ||
                                 (usb.keyboardFd < 0 && usb.mediaFd < 0))
                   ? "keyboard actions require USB Keyboard enabled and a "
                     "connected host"
               : needsStandard && usb.keyboardFd < 0
                   ? "keyboard actions require a connected keyboard interface"
               : needsMedia && usb.mediaFd < 0
                   ? "media actions require USB Consumer Control and a "
                     "connected host"
                   : "MIDI and light actions require a connected controller");
      printf("[SCRIPT] [WARN] Macro %s failed: %s\n", macro.name,
             macroErrors[activeMacro]);
      activeMacro = -1;
      return;
    }
    macroStates[activeMacro] = nextState;
    macroStep = 0;
    macroWakeMs = now;
    macroDeadlineMs = now + 65000;
    macroWaitingTap = false;
    macroTextIndex = 0;
    macroTextPending = false;
    macroWaitingController = false;
  }
  if (now >= macroDeadlineMs) {
    failMacro("macro timed out; cancelling pending actions");
    return;
  }
  if (macroUsesKeyboard[activeMacro] &&
      (!midi.keyboardEnabled || (usb.keyboardFd < 0 && usb.mediaFd < 0))) {
    failMacro("USB keyboard unavailable");
    return;
  }
  if (macroWaitingController && controllerPendingSize)
    return;
  macroWaitingController = false;
  if (macroWaitingTap && (usb.chordCount || (usb.keyDown && !usb.keyHeld)))
    return;
  macroWaitingTap = false;
  if (macroTextPending) {
    const auto &action = macroPlan.actions[macroStep];
    if (action.text[macroTextIndex])
      macroWakeMs = usb.keyboardPressMs + action.value;
    macroTextPending = false;
  }
  if (now < macroWakeMs)
    return;
  for (unsigned budget = 0;
       budget < SCRIPT_ACTIONS_MAX && macroStep < macroPlan.count; ++budget) {
    const auto &action = macroPlan.actions[macroStep];
    if (action.kind == ScriptKind::Tap || action.kind == ScriptKind::Type) {
      if (usb.keyHeld) {
        if (!usb.release()) {
          failMacro("held keyboard/media key could not be released");
          return;
        }
        macroWaitingTap = true;
        return;
      }
      if (action.kind == ScriptKind::Type) {
        if (!action.text[macroTextIndex]) {
          macroTextIndex = 0;
          ++macroStep;
          continue;
        }
        uint8_t report[8];
        if (!scriptAsciiReport(action.text[macroTextIndex], report) ||
            !usb.queueKeyboardReport(report)) {
          failMacro("typing report could not be queued");
          return;
        }
        ++macroTextIndex;
        macroTextPending = true;
      } else {
        if (!usb.trigger(action.keys)) {
          failMacro("keyboard/media report could not be queued");
          return;
        }
        ++macroStep;
      }
      macroWaitingTap = true;
      return;
    }
    ++macroStep;
    if (action.kind == ScriptKind::Press ||
        action.kind == ScriptKind::Release) {
      bool queued = action.kind == ScriptKind::Press ? usb.press(action.keys)
                                                     : usb.release();
      if (!queued) {
        failMacro("keyboard report could not be queued");
        return;
      }
      macroWaitingTap = true;
      return;
    }
    if (action.kind == ScriptKind::Wait) {
      macroWakeMs = now + action.value;
      if (action.value)
        return;
    } else if (action.kind == ScriptKind::Light) {
      macroLight[activeMacro] = action.value;
      midi.refreshLightingFromState();
    } else if (action.kind == ScriptKind::Log) {
      printf("[SCRIPT] [LUA] ch=%u note=%u %s\n",
             compiledMacros[activeMacro].channel,
             compiledMacros[activeMacro].note, action.text);
    } else if (action.kind == ScriptKind::ConfigWrite) {
      if (!midi.app || !midi.app->config ||
          midi.app->config->writeScriptConfig(
              action.target, macroPlan.configText + action.value,
              static_cast<size_t>(action.number)) != RET_OK) {
        failMacro(
            "config.write failed; check file format, storage and write limits");
        return;
      }
    } else if (action.kind == ScriptKind::SettingsSet) {
      if (!midi.app || !midi.app->config ||
          midi.app->config->writeScriptSetting(action.target, action.setting) !=
              RET_OK) {
        failMacro("settings.set failed; check settings and persistent storage");
        return;
      }
    } else if (action.kind == ScriptKind::StorageWrite ||
               action.kind == ScriptKind::StorageRemove ||
               action.kind == ScriptKind::ScriptConfigSet) {
      ScriptStorage storage(compiledMacros[activeMacro].id);
      int result = RET_ERR;
      if (action.kind == ScriptKind::StorageWrite)
        result = storage.writeFile(action.target,
                                   macroPlan.configText + action.value,
                                   static_cast<size_t>(action.number));
      else if (action.kind == ScriptKind::StorageRemove)
        result = storage.removeFile(action.target);
      else
        result = storage.setSetting(action.setting, action.value == 0);
      if (result != RET_OK) {
        char error[192];
        snprintf(error, sizeof(error), "Script storage action failed: %s",
                 strerror(errno));
        failMacro(error);
        return;
      }
    } else if (action.kind == ScriptKind::SystemRestart) {
      printf("[SCRIPT] [LUA] Script %s requested device restart\n",
             compiledMacros[activeMacro].name);
      flushLogs();
      sync();
      if (reboot(RB_AUTOBOOT) < 0) {
        char error[192];
        snprintf(error, sizeof(error), "system.restart failed: %s",
                 strerror(errno));
        failMacro(error);
        return;
      }
    } else if (action.kind == ScriptKind::BluetoothScan ||
               action.kind == ScriptKind::BluetoothPair ||
               action.kind == ScriptKind::BluetoothConnect ||
               action.kind == ScriptKind::BluetoothDisconnect ||
               action.kind == ScriptKind::BluetoothUnpair ||
               action.kind == ScriptKind::BluetoothAutoConnect) {
      auto *bluetooth = midi.app ? midi.app->bluetooth : nullptr;
      int result = RET_ERR;
      if (bluetooth) {
        switch (action.kind) {
        case ScriptKind::BluetoothScan:
          result = bluetooth->requestScan();
          break;
        case ScriptKind::BluetoothPair:
          result = bluetooth->requestPair(action.target);
          break;
        case ScriptKind::BluetoothConnect:
          result = bluetooth->requestConnect(action.target);
          break;
        case ScriptKind::BluetoothDisconnect:
          result = bluetooth->requestDisconnect(action.target);
          break;
        case ScriptKind::BluetoothUnpair:
          result = bluetooth->requestUnpair(action.target);
          break;
        case ScriptKind::BluetoothAutoConnect:
          result =
              bluetooth->requestAutoConnect(action.target, action.value != 0);
          break;
        default:
          break;
        }
      }
      if (result != RET_OK) {
        failMacro(result == RET_WARN
                      ? "Bluetooth request rejected: busy or unavailable device"
                      : "Bluetooth request failed: unavailable adapter or "
                        "invalid address");
        return;
      }
    } else if (action.kind == ScriptKind::SoundPlay) {
      if (!midi.app || !midi.app->audio ||
          midi.app->audio->triggerPreloadedSfxPitch(
              action.text, static_cast<float>(action.pitchRatio)) != RET_OK) {
        failMacro(
            "sound.play failed; ensure the WAV is uploaded and preloaded");
        return;
      }
    } else if (action.kind == ScriptKind::SoundStopAll) {
      if (!midi.app || !midi.app->audio) {
        failMacro("sound.stop_all failed; audio unavailable");
        return;
      }
      midi.app->audio->stopAllSfx();
    } else if (action.kind == ScriptKind::EffectEnable ||
               action.kind == ScriptKind::EffectToggle ||
               action.kind == ScriptKind::EffectSet) {
      auto *audio = midi.app ? midi.app->audio : nullptr;
      int result = RET_ERR;
      if (audio) {
        if (action.kind == ScriptKind::EffectEnable)
          result = audio->setEffectEnabled(action.target, action.value);
        else if (action.kind == ScriptKind::EffectToggle)
          result = audio->toggleEffectEnabled(action.target);
        else {
          audiox::effects::SlotParams params{};
          if (audio->getEffectParams(action.target, &params) == RET_OK) {
            const auto *spec = audiox::effects::effectParamSpecFor(
                params.type, action.parameter);
            if (spec && action.number >= spec->minValue &&
                action.number <= spec->maxValue)
              result = audio->setEffectParam(action.target, action.parameter,
                                             static_cast<float>(action.number));
          }
        }
      }
      if (result != RET_OK) {
        failMacro("effect action failed; check effect ID, parameter and range");
        return;
      }
      midi.refreshLightingFromState();
    } else if (action.kind == ScriptKind::VolumeSet ||
               action.kind == ScriptKind::VolumeAdjust) {
      auto *audio = midi.app ? midi.app->audio : nullptr;
      bool found = false;
      if (audio) {
        std::lock_guard<std::mutex> lock(audio->routingGraphMutex);
        for (unsigned i = 0; i < audio->routingGraph.thingCount; ++i)
          if (!strcmp(audio->routingGraph.things[i].id, action.target))
            found = true;
      }
      if (!found) {
        failMacro("volume action failed; unknown audio thing ID");
        return;
      }
      double percent = action.number;
      if (action.kind == ScriptKind::VolumeAdjust)
        percent += audio->getThingGain(action.target) * 100;
      audio->setThingGain(
          action.target,
          static_cast<float>(std::clamp(percent, 0.0, 100.0) / 100));
    } else if (action.kind == ScriptKind::Midi) {
      if (!queueControllerMidi(action.midiBytes, action.midiSize)) {
        failMacro("controller MIDI output failed or its queue is full");
        return;
      }
      unsigned kind = action.midiBytes[0] & 0xf0,
               channel = action.midiBytes[0] & 15;
      if (action.midiSize == 3 && (kind == 0x80 || kind == 0x90)) {
        macroMidiNotes[channel][action.midiBytes[1]] =
            kind == 0x90 && action.midiBytes[2] != 0;
      } else if (action.midiSize == 3 && kind == 0xb0 &&
                 action.midiBytes[1] == 64) {
        macroMidiSustain[channel] = action.midiBytes[2] >= 64;
      }
      if (controllerPendingSize) {
        macroWaitingController = true;
        return;
      }
    }
  }
  if (macroStep == macroPlan.count) {
    if (usb.keyHeld) {
      (void)usb.release();
      macroWaitingTap = true;
      return;
    }
    releaseMacroMidi();
    macroLight[activeMacro] = -1;
    activeMacro = -1;
    midi.refreshLightingFromState();
  }
}

bool ScriptContext::flushControllerMidi() {
  if (!controllerPendingSize)
    return !controllerOutputFailed;
  if (midi.fd < 0 || !midi.connected || controllerOutputFailed)
    return false;
  ssize_t n = write(midi.fd, controllerPending, controllerPendingSize);
  if (n > 0) {
    controllerPendingSize -= static_cast<size_t>(n);
    memmove(controllerPending, controllerPending + n, controllerPendingSize);
    return true;
  }
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    return true;
  printf("[SCRIPT] [WARN] Controller MIDI write failed: %s\n",
         n < 0 ? strerror(errno) : "zero-length write");
  controllerOutputFailed = true;
  return false;
}

bool ScriptContext::queueControllerMidi(const uint8_t *bytes, size_t count) {
  if (midi.fd < 0 || !midi.connected || !count || !flushControllerMidi())
    return false;
  if (count > sizeof(controllerPending) - controllerPendingSize) {
    printf("[SCRIPT] [WARN] Controller MIDI output queue full\n");
    return false;
  }
  memcpy(controllerPending + controllerPendingSize, bytes, count);
  controllerPendingSize += count;
  return flushControllerMidi();
}

void ScriptContext::releaseMacroMidi() {
  for (unsigned channel = 0; channel < 16; ++channel) {
    if (macroMidiSustain[channel]) {
      uint8_t off[] = {static_cast<uint8_t>(0xb0 | channel), 64, 0};
      if (!queueControllerMidi(off, sizeof(off)))
        printf("[SCRIPT] [WARN] Cannot release macro MIDI sustain\n");
      macroMidiSustain[channel] = false;
    }
    for (unsigned note = 0; note < 128; ++note) {
      if (!macroMidiNotes[channel][note])
        continue;
      uint8_t off[] = {static_cast<uint8_t>(0x80 | channel),
                       static_cast<uint8_t>(note), 0};
      if (!queueControllerMidi(off, sizeof(off)))
        printf("[SCRIPT] [WARN] Cannot release macro MIDI note\n");
      macroMidiNotes[channel][note] = false;
    }
  }
}
