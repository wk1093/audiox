# Events and persistent state

AudioX evaluates each named script once for each queued web Run or MIDI note-on. It
provides a fresh `event` table and the mapping's persistent `state` table.
Lua locals and ordinary globals are recreated on every evaluation; only values
written to `state` are retained.

## The triggering event

| Field | Meaning |
| --- | --- |
| `event.source` | `"web"` for Run, `"midi"` for MIDI note-on. |
| `event.channel` | MIDI source channel, `0`–`15`. |
| `event.note` | MIDI note number, `0`–`127`. |
| `event.velocity` | Note-on velocity, `1`–`127`. |
| `event.pressed` | `true`; MIDI note-off does not run scripts. |

Web triggers still supply numeric channel/note from the macro's configuration
and velocity `127`, including when the MIDI binding is disabled.

For example, use the velocity to choose a light level:

```lua
local level = math.floor(event.velocity / 2)
light(level)
```

## Clock snapshots

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
Editing/removing a macro, cancellation or restarting AudioX resets its state.
USB loss and keyboard disable no longer globally reset non-keyboard scripts:
only plans requiring keyboard actions need that host session. Light/MIDI
plans depend on the selected connected controller. A playback failure such as a missing sound
or unavailable controller output cancels pending macros and resets state;
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
