# Scripts

The **Scripts** page stores up to **16 named Lua scripts**. A script registers
event callbacks with `hook()`. Triggers include Web Run, MIDI note/CC events,
sound voice start/finish, and selected controller connect/disconnect events.
See [Lua scripting](/lua/) for callback behavior and the complete API.

## Create and run a script

1. Choose **New Script**, enter a name (1–63 UTF-8 bytes), and edit the source.
2. Choose **Save Script**.
3. Choose **Run** beside the saved script.

Run queues the saved source; it does not execute unsaved editor text or confirm
completion. It is unavailable unless the script registers a `run` callback.
Choose **Reload Scripts** to refresh runtime errors. Editing preserves the
script's stable ID; **Remove** deletes its definition.

Scripts register up to 32 callbacks. Setup code runs in a fresh environment for
each trigger; ordinary variables reset. Only supported scalar values assigned
to `state` persist in RAM. Register hooks unconditionally at top level; access
`event`, `state`, clocks, configuration, and device actions inside callbacks.
Each trigger evaluates atomically into a bounded action plan. See
[events and state](/lua/events-and-state) and
[limits and troubleshooting](/lua/limits-and-troubleshooting).

## Trigger from MIDI

Enable **Bind to a MIDI pad**, then enter channel (0–15) and note (0–127), or
choose **Learn MIDI Pad** and press the control. Learning sets channel and note;
save the script after editing. Register `note_on`, `note_off`, or `cc` callbacks
as needed. For CC, the Note field is the controller number.

Two scripts cannot use overlapping MIDI subscriptions on the same binding.
Web-only scripts reserve no MIDI trigger. Script bindings coexist with sound,
action, and effect mappings. Note-off triggers a new evaluation; it does not
cancel an existing plan. Sound and controller connection callbacks do not
require a pad binding.

## Keyboard, MIDI, and lighting requirements

| Action | Requirement |
| --- | --- |
| `tap`, `press`, `release`, `type` | USB Keyboard enabled and USB host connected |
| `light` | Enabled MIDI binding and selected controller connected |
| `midi.*` | Selected controller connected |
| `sound.*` | Audio subsystem and named clip loaded |
| `effects.*`, `volume.*` | Existing effect or routing thing ID |
| Calculations, `state`, logging | No attached host or controller |

Keyboard output reaches the connected USB host's focused application, not
necessarily the computer running the browser. Enable **USB Keyboard** in
[Config](/guide/config) and reload USB configuration before using keyboard
actions. The host layout affects `type()`; see
[keyboard behavior](/lua/keyboard-and-lighting).

Set idle/pressed velocities for a MIDI-bound script; 0 means off and 1–127 is
controller-specific. The output channel is set under **MIDI > Lighting**.
Script colors override other mapped colors on the same note; **Dim lights**
overrides all output. `light()` temporarily changes the bound pad and is
restored when playback ends or is canceled. Web-only scripts should keep both
default velocities at zero.

## Runtime and configuration

Definitions persist across reboot; `state` does not. Edits, cancellation,
failure, controller disconnect, or restart can reset state. USB loss resets
keyboard-dependent work, not unrelated scripts. Evaluation failure discards
that event's actions and state changes; already-delivered actions cannot be
undone if playback later fails.

Per event, limits include **17,408 UTF-8 source bytes**, 100,000 instructions,
1 MiB Lua memory, 128 actions, 1,024 typed characters, 60 seconds of scheduled
duration, and 16 pending triggers. `wait()` delays playback, not evaluation.
Lua does not run on the audio thread.

Effects and volume actions are runtime-only. Configuration APIs can read or
replace allowlisted text files. Writes require explicit **Allow configuration
writes** permission and confirmation when saving the script. Writes are raw
replacements, not format-validated updates; back up first and reload or restart
the relevant subsystem as needed. See [Config](/guide/config) and the
[configuration API](/lua/api#persistent-configuration).

## HTTP integration

Use `/api/scripts` to list, create, or update named callback scripts and
`/api/scripts/run` to queue a saved `run` callback. The currently supported
`/api/macros` and `/api/midi/chords` routes remain compatibility endpoints; new
clients can use the script routes. See the [HTTP reference](/guide/reference#named-scripts).

Continue with [Lua getting started](/lua/getting-started), the
[API reference](/lua/api), and the [cookbook](/lua/cookbook).
