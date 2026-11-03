# Events and persistent state

AudioX evaluates each named script once for each queued trigger. It
provides a fresh `event` table and the mapping's persistent `state` table.
Lua locals and ordinary globals are recreated on every evaluation; only values
written to `state` are retained.

## Register callbacks in one script

Lua functions are values, so pass a
function to `hook(eventName, callback)` for each behavior:

```lua callbacks
local scratch = 0

hook("run", function(event)
    scratch = scratch + 1
    state.runs = (state.runs or 0) + 1
    log("Run " .. state.runs)
end)

hook("sound_finish", function(event)
    if event.sound == "intro.wav" then
        log("Intro finished")
    end
end)
```

Subscriptions come from these calls, not editor checkboxes. Script setup reruns
in a fresh environment for each event, then only matching callbacks execute.
In this example, `scratch` starts at zero on every event; only `state.runs`
remembers previous events. Ordinary globals, local tables and closure variables
also reset. No functions or tables can be stored in `state`.

Up to 32 callbacks may be registered, including several for the same event.
Matching handlers run in registration order, sharing that event's temporary
environment and one aggregate action plan. If any handler fails, **none** of the
event's queued actions or state updates are committed. Earlier events are not
rolled back. `wait()` queues playback delays; it does not yield a callback.

Register hooks unconditionally at top level. Setup may define helper functions,
tables and constants, but `event` and `state` are provided **after** registration;
use them inside handlers, not in top-level initializers. Configuration reads,
writes, clock snapshots and queued device actions also belong inside handlers.
Save-time registration validates names, handler types and limits without
calling handlers or performing device I/O. Registering hooks inside a callback
is an error. Conditional/dynamic subscriptions are not supported.

Add a `"run"` hook to enable the Web Run button; Web Run does not invoke
`"note_on"` handlers. MIDI hooks still require the pad binding
and match its channel/note (controller number for CC). Sound and controller
connection hooks do not require a binding.

## The triggering event

| Field | Meaning |
| --- | --- |
| `event.type` | MIDI/sound/controller events, `run`, `boot`, `timer`, `config_change`, `ui`, or `bluetooth_change`; see the [API](./api#startup-and-utility-hooks). |
| `event.source` | `"web"` for Run/options/buttons, `"midi"` for MIDI, `"audio"` for sounds, `"device"` for the MIDI controller, `"system"` for boot/timer, `"bluetooth"` for Bluetooth changes. |
| `event.channel` | MIDI source channel, `0`–`15`. |
| `event.note` | MIDI note number, `0`–`127`. |
| `event.velocity` | MIDI velocity/value, `0`–`127`; zero for audio/device hooks, `127` for Web Run. |
| `event.pressed` | True for note press, sound start, controller connect and Web Run; false for note release, CC, sound finish and disconnect. |
| `event.cc`, `event.value` | CC controller number and value, `0`–`127`; `value` mirrors MIDI velocity except for `config_change` (typed option value) and `ui` (nil). |
| `event.key` | Changed option or pressed custom button key; empty for other events. |
| `event.sound` | Sound basename for audio hooks; empty otherwise. |
| `event.device` | Selected controller device path for connection hooks; empty otherwise. |

Web triggers still supply numeric channel/note from the script's configuration
and velocity `127`, including when the MIDI binding is disabled.
Web Run supplies `event.type == "run"`.

Register subscriptions using `hook()` in the script editor.
MIDI press, release (including
zero-velocity note-on) and CC subscriptions match the configured channel and
Note field; for CC that field is the controller number. Separate scripts may
share a binding only if their MIDI event subscriptions do not overlap.
Sound and connection hooks do not require a MIDI binding.

Sound events are per voice, including overlapping playback and clips that start
and finish in one audio block. Finish also fires on explicit stop or voice
replacement. `event.sound` identifies the clip, not a unique voice. Lua never
runs on the audio thread: an audio-to-MIDI ring transfers up to 128 pending
events; overflow is logged and excess events are dropped.

`device_connect`/`device_disconnect` cover the **selected MIDI controller**, not every USB,
Bluetooth or audio device. Use `bluetooth_change` for Bluetooth state snapshots.
MIDI disconnect first cancels queued scripts and resets
their state, then queues disconnect subscribers. These scripts can still use
configuration, effects and logging, but cannot send to the disconnected controller.

Audio/device hooks have channel/note/velocity zero. Web Run uses the configured
channel/note. Clock snapshots are captured
when the MIDI poll loop receives the hook, not at sample-accurate audio time.
Filter `event.sound` and `event.type`, and guard recursive playback: a
sound-start script that plays another sound can continually retrigger itself.

For example, use the velocity to choose a light level:

```lua
local level = math.floor(event.velocity / 2)
light(level)
```

## Clock snapshots

Boot hooks wait for 60 healthy seconds after runtime readiness, including
ordinary boots. Timer hooks then run once per second, coalescing pending ticks.
They use the same trigger queue as MIDI/sound/UI hooks and are not hard realtime.
`state` is still RAM-only; use `script_config` for values that must survive
restart and `storage` for custom persistent files. Neither can access official
device configuration. See [script-owned configuration](./api#script-owned-configuration-and-webui).

`millis()` returns the system monotonic clock in milliseconds captured when
the original trigger arrived. It is suitable for elapsed-time comparisons and
does not jump when wall-clock time is adjusted. Its origin is unspecified
(typically time since boot); at the extremely unlikely point where the clock exceeds the signed 64-bit range,
the reported value saturates.

`epoch()` returns Unix time in whole seconds captured at that same trigger.
The device clock may not have been set yet, for example early in boot without
network time. Neither helper advances during script generation, `wait()`, or
queued playback. Call them in a later event to obtain a newer snapshot.

## Retaining simple values

`state` holds up to **32 named fields**. Keys are strings of 1–31 bytes.
Values must be booleans, finite numbers, or strings up to 127 bytes. It is a
flat store: tables and other Lua values cannot be saved. Remove a field by
assigning `nil`.

```lua
state.count = (state.count or 0) + 1
log("trigger " .. state.count)
```

Use a persistent counter to alternate behavior:

```lua
state.count = (state.count or 0) + 1
if state.count % 2 == 0 then
    tap("F13")
else
    tap("F14")
end
```

Successful evaluations commit their state changes. A failed evaluation leaves
the previous state intact, even if it assigned fields before the error.
Editing/removing a script, cancellation or restarting AudioX resets its state.
USB loss and keyboard disable no longer globally reset non-keyboard scripts:
only plans requiring keyboard actions need that host session. Light/MIDI
plans depend on the selected connected controller. A playback failure such as a missing sound
or unavailable controller output cancels pending scripts and resets state;
actions that have already happened cannot be undone.

## Evaluation is not playback

Lua control flow, arithmetic, and state updates happen during evaluation to
produce a complete bounded plan. The queued plan is then replayed without
running Lua again. As a result:

```lua
wait(1000)
log(tostring(millis()))
```

does not log a clock value one second later: the call and its argument are
evaluated before the wait begins. Put time-dependent decisions in a later
note-on evaluation instead.
