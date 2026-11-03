# Scripts and automation

The **Scripts** page holds up to **16 named Lua scripts**. Each script can
register several callbacks with `hook("event_name", function(event) ... end)`.
Use them for Web Run, MIDI presses/releases and CC, sound voice start/finish,
and selected MIDI controller connect/disconnect events.

## Create and run a script

1. Open **Scripts** and choose **New Script**.
2. Enter a recognizable name, up to **63 UTF-8 bytes**.
3. Edit the default `run` callback, then choose **Save Script**.
4. Choose **Run** beside the saved entry.

Run queues the **saved version**, not the unsaved editor, and confirms queuing,
not completion. It is disabled without a `run` hook. Use **Reload Scripts** to
see execution errors and check [System logs](./system) for context.
**Edit** preserves the stable ID, even when changing the name or binding.
**Remove** deletes that definition.

## Existing shortcuts and sequences

The old Keyboard chord and Lua sequence editor modes have been removed.
Existing saved entries are automatically converted to callback source when the
MIDI map is loaded. Keyboard shortcuts become `tap("...")` calls. Sequences
keep their source inside a generated function registered for their previous
subscriptions and Web Run. The wrapper preserves the old note-on-shaped
`event.type` for Run. IDs, names, bindings, LED colors, and configuration-write
permissions are retained; conversion never grants new permission.

Conversion happens in memory. The converted source is persisted on the next
successful MIDI-map/settings save, not by rewriting files during reads.
Existing callback scripts are unchanged. Invalid entries are logged and
disabled rather than silently discarded.

The old `/api/macros`, `/api/macros/run`, and `/api/midi/chords` routes remain
compatibility import bridges. New integrations should use `/api/scripts` and
`/api/scripts/run`. The list response is `{"scripts":[...]}`; save requests
use named entries and callback source, not shortcut or sequence modes.

## Keyboard shortcuts

Put `tap("CTRL+SHIFT+F13")` inside a callback. Enable **USB Keyboard** in
[Config](./config), apply the configuration, and connect AudioX's USB gadget
port to the target computer. Output goes to the **USB host**, not necessarily
the machine running the browser, and affects its focused application.
There is no separate shortcut recorder or shortcut editor mode.

## Add a MIDI trigger

Check **Bind to a MIDI pad**, then enter channel (0-15) and note (0-127), or
choose **Learn MIDI Pad** and press it. Add `hook("note_on", callback)` for
presses, `note_off` for releases, or `cc` for control changes. For CC the Note
field is the controller number. Save after learning.

Two scripts cannot share overlapping MIDI subscriptions on the same binding.
Web-only entries reserve no MIDI trigger. Script bindings do not remove
separate sound/action mappings: one pad can still run both. Note release
queues another evaluation; it does not automatically cancel an existing plan.
Sound/controller hooks need no pad binding.

## Device requirements and lighting

| Action | Requirements |
| --- | --- |
| `tap`, `press`, `release`, `type` | USB Keyboard enabled and connected USB host |
| `light` | Enabled MIDI binding and selected controller connected |
| `midi.*` | Selected controller connected |
| `sound.*` | Audio subsystem and named clip already loaded |
| `effects.*`, `volume.*` | Existing effect/audio thing IDs |
| Calculations, `state`, logging | No USB host or MIDI controller required |

Set idle/pressed LED velocities for MIDI-bound scripts: zero means off,
1-127 selects the controller's color/velocity. The global output channel
is configured on **MIDI > Lighting**. Script colors override other mapped
colors on the same note; **Dim lights** still wins. Temporary `light()` output
is restored after playback or cancellation. Leave default velocities at zero
for web-only scripts. Calling `light()` without a binding reports an error.

## Lifetime and safety

Ordinary variables reset per event; only explicitly assigned scalar `state`
values persist in RAM. Register hooks unconditionally at top level; put
actions, event/state access, clocks, and configuration I/O inside callbacks.
Up to 32 handlers may share a script and run in registration order.
Saving inspects registration without executing handlers.

Each event evaluates atomically into a bounded action plan: **17,408 UTF-8
source bytes** including migration wrappers, 100,000 instructions, 1 MiB Lua
memory, 128 actions, 1,024 typed characters, and 60 seconds scheduled duration.
`wait()` delays playback, not evaluation. Lua never runs on the audio thread.
Evaluation failure discards that event's pending actions and state changes.
Already-delivered physical actions cannot be undone if playback later fails.

Definitions survive reboot; `state` does not. Edits, cancellation/failure,
controller disconnect and restart may reset it. USB session changes reset
keyboard-using scripts, not all scripts globally.

Effects and volume actions are runtime-only. Configuration APIs can instead
read or replace allowlisted persistent files; saving requires confirmation.
**Allow configuration writes** explicitly requests write permission, including
for dynamic API access. Raw edits are not format-validated and may require
reload/restart. Back up first.

- [Getting started](/lua/getting-started)
- [Events and state](/lua/events-and-state)
- [API reference](/lua/api)
- [Cookbook](/lua/cookbook)
- [Limits and troubleshooting](/lua/limits-and-troubleshooting)
