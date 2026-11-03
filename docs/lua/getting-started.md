# Getting started with Scripts

Scripts are named Lua programs that respond to Web Run, MIDI, sound, or
controller events. They run on AudioX, not in the browser. A script can play a
loaded sound, control an effect or volume, send MIDI to a controller, change a
bound pad's light, or send keyboard reports to the USB host.

## Create and run a script

1. Open **Scripts** and choose **New Script**.
2. Enter a name of 1–63 UTF-8 bytes and paste:

   ```lua
   hook("run", function(event)
       state.runs = (state.runs or 0) + 1
       log("Run " .. state.runs)
   end)
   ```

3. Choose **Save Script**, then **Run**.

Run queues the saved source; it does not run unsaved edits or confirm
completion. Reload the script list to see runtime errors. Ordinary variables
reset on each event. Only supported values assigned to `state` persist in RAM.
Register hooks at top level; use `event`, `state`, device actions, and clocks
inside callback functions. See [events and state](/lua/events-and-state).

AudioX supports up to 16 named scripts and 32 callbacks per script. Web Run
requires a `run` callback. For controller triggers, enable **Bind to a MIDI
pad**, enter the channel (0–15) and note (0–127), or choose **Learn MIDI Pad**.
Register `note_on`, `note_off`, or `cc` callbacks as appropriate; for CC, the
Note field is the controller number. Save after learning.

To share a handler between Web Run and a bound pad:

```lua callbacks
local function report(event)
    log("note " .. event.note .. ", velocity " .. event.velocity)
end

hook("run", report)
hook("note_on", report)
```

MIDI release is a separate event; it does not cancel an action plan already
queued by note-on. Script bindings coexist with sound/action mappings. See the
[MIDI guide](/guide/midi) for mapping precedence.

## Choose required devices

| Action | Requirement |
| --- | --- |
| `tap`, `press`, `release`, `type` | USB Keyboard enabled and USB host connected |
| `light` | Enabled MIDI binding and selected controller connected |
| `midi.*` | Selected controller connected |
| `sound.*` | Audio subsystem and named clip loaded |
| `effects.*`, `volume.*` | Existing effect or routing thing ID |
| Calculations, `state`, logging | No host or controller |

Enable **USB Keyboard** in [Config](/guide/config) only for keyboard actions.
The host application's focused window receives these physical key reports;
`type()` supports printable US-ASCII plus newline and tab. Set the host layout
to US with Caps Lock off. Keyboard actions are not required for sound, effect,
volume, or logging scripts.

`light()` acts on the script's bound controller pad. It needs a saved MIDI
binding and the selected controller, including when started by Web Run. For a
web-only script, leave its idle and pressed LED velocities at zero.

## Evaluation and playback

Each trigger evaluates Lua to build a bounded action plan, then plays that plan
separately. `wait()` delays playback; it does not pause Lua or update event and
clock values. Evaluation failure discards that event's pending actions and
state changes. Playback failure cancels pending work, but cannot undo physical
actions already delivered.

Sounds must already be loaded in AudioX. MIDI output goes to the selected
controller, not the USB host. Effects and volume changes are runtime-only.
Scripts cannot change routes. Read the [API reference](/lua/api), the
[cookbook](/lua/cookbook), and [limits and troubleshooting](/lua/limits-and-troubleshooting)
before building longer plans.
