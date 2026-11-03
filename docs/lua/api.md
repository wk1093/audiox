# Lua API reference

Scripts use Lua 5.4 syntax in a restricted environment. The API is intentionally
small: there is no arbitrary filesystem, shell, network, dynamic loading, or unrestricted
device access. `_G`, `_VERSION`, metatables, pattern matching, module loading,
coroutines, `io`, `os`, `debug`, and `package` are not exposed. Each function
below is available globally unless shown under
`math`, `string`, `table`, `sound`, `midi`, `effects`, `volume`, or `config`.
Unless shown as complete callback scripts, examples below are handler-body
snippets: put them inside `hook("run", function(event) ... end)` or another
event callback, not at top level.

## Trigger and state

| Name | Description |
| --- | --- |
| `event.type` | `note_on`, `note_off`, `cc`, `sound_start`, `sound_finish`, `device_connect`, `device_disconnect`, or `run`. |
| `event.source` | `"web"`, `"midi"`, `"audio"` or `"device"`; all hooks use the same bounded queue. |
| `event.channel` | Triggering MIDI channel, `0`–`15`. |
| `event.note` | Triggering note, `0`–`127`. |
| `event.velocity` | MIDI velocity/value, `0`–`127`; zero for audio/device hooks. |
| `event.pressed` | True for press/start/connect and Web Run; false for release/CC/finish/disconnect. |
| `event.cc`, `event.value` | MIDI controller number and value; `value` mirrors velocity for notes. |
| `event.sound`, `event.device` | Sound basename or selected controller device path; empty when inapplicable. |
| `state` | Persistent flat table for this mapping; see [events and state](/lua/events-and-state). |

## Callback registration

`hook(eventName, callback)` registers a script's event handlers. Event names
are the eight `event.type` values above; the second argument must be a Lua
function. Register up to 32 handlers at top level. Matching functions receive
the fresh event table as their sole argument and run in registration order.
Multiple handlers for the same event are supported; return values are ignored.

Setup reruns on each event; ordinary globals, locals and closures do **not**
persist. Use `state` explicitly inside callbacks to remember scalar values.
State and event are unavailable during registration. Device actions, configuration
I/O and clock snapshots are callback-only. Registration is inspected in a
bounded sandbox at save time to derive subscriptions, without invoking handlers.
Register unconditionally, not inside handlers or based on changing values.
Setup and all matching handlers share instruction/memory/action budgets.
Any evaluation error discards the event's entire plan and state changes.

The Web Run button invokes `hook("run", callback)` only. Without a run hook it
is disabled and the Run API rejects execution. Automatically converted
sequences preserve their previous note-on-shaped event inside the generated wrapper.
See [callback examples and lifetime rules](/lua/events-and-state#register-callbacks-in-one-script).

## Persistent configuration

| Call | Description |
| --- | --- |
| `config.list()` | Array of six allowed file names. |
| `config.read(name)` | Read a regular, non-symlink UTF-8/raw text file, up to 65,536 bytes, with no NUL bytes. Missing or invalid files raise an error. |
| `config.lines(name)` | Read the same file as a dense line array, without LF/CRLF terminators. At most 1,024 lines, 4,096 bytes per line; empty files yield an empty array. |
| `config.write(name, text)` | Queue a complete file replacement, up to 4,096 bytes, without NUL or non-whitespace control characters. Requires saved write permission. |

Allowed names are `config.txt`, `config.staging.txt`, `routing.txt`,
`midi_map.txt`, `fx_params.txt`, and `volumes.txt`. Paths and arbitrary files
are rejected. Bluetooth pairing storage is excluded because it contains credentials.

Reads happen during evaluation and are limited to four file reads per trigger
(`read` and `lines` combined). Different reads are not a transactional snapshot.
Use `config.lines` to process large files with the bounded string helpers:
the ordinary string-operation limit remains 4,096 bytes.

The editor warns before saving scripts containing `config`, including read-only
scripts and comments. **Allow configuration writes** explicitly requests
permission for dynamically accessed APIs. Every permission-granting save
requires confirmation; cancellation sends no request. The device persists that
permission with the script and rejects writes without it, even through aliases.

Writes execute only after successful evaluation, alongside other queued actions.
Payloads share an 8,192-byte per-plan buffer, including one NUL terminator per
write. An evaluation failure discards all pending writes. Each successful write
is an atomic, flushed replacement; multiple files/actions are **not** one
transaction, and an execution failure does not undo earlier successful actions.

These are raw text edits, **not format-validated settings changes**. Back up
first, preserve the documented file format, and reload/restart the relevant
subsystem afterwards. Some subsystems periodically reload their files.
Editing `midi_map.txt` can change or reset scripts at the next map reload.
Avoid writes on every CC or sound event: filesystem sync can stall the control
loop and repeated writes wear flash storage. Use runtime `effects`/`volume`
actions when persistence is unnecessary.

## Keyboard, timing, and logging

| Call | Description |
| --- | --- |
| `tap(keys)` | Tap a chord and release it. Example: `tap("CTRL+A")`. |
| `press(keys)` | Replace the currently held chord. Modifier-only chords are allowed. |
| `release()` | Release all keys held by the script. |
| `type(text, interval_ms)` | Type supported US-ASCII text. The interval defaults to 50 ms. |
| `wait(ms)` | Add a playback delay, `0`–`60000` ms. |
| `light(level)` | Temporarily set the bound pad light, `0`–`127`. Requires an enabled MIDI binding and the selected connected controller, including for web Run; no binding is an explicit runtime error. |
| `millis()` | Monotonic milliseconds captured at the original trigger. |
| `epoch()` | Unix seconds captured at the original trigger; the device clock may be unset. |
| `log(message)` | Queue a string as a device diagnostic message, at most 255 bytes and no NUL. |

Keyboard specifics and playback behavior are in
[keyboard and lighting](/lua/keyboard-and-lighting).

## MIDI output

MIDI output goes to the connected MIDI controller, not to the USB host. Output
is bounded and nonblocking; unavailable or invalid output is reported as an
error. Raw messages must be complete valid MIDI short messages and use valid
7-bit data bytes.

| Call | Description |
| --- | --- |
| `midi.send(status[, data1[, data2]])` | Send a complete short MIDI message. Supply exactly the data bytes required by its status. |
| `midi.note_on(channel, note, velocity)` | Send a channel note-on. |
| `midi.note_off(channel, note, velocity)` | Send a channel note-off; velocity defaults to `0`. |
| `midi.cc(channel, controller, value)` | Send a Control Change. |
| `midi.program(channel, program)` | Send a Program Change. |

Channel values are `0`–`15`; note, controller, program, velocity, and data
values are 7-bit (`0`–`127`). Note-on velocity zero has the standard MIDI
meaning of note-off. `midi.note_on()` does not schedule a note-off in the
script; pair it with `midi.note_off()`. See
[MIDI and soundboard](/lua/midi-and-soundboard) for safe timing examples.

Raw `midi.send()` supports channel messages (`0x80`–`0xEF`), the defined
system-common statuses `0xF1`, `0xF2`, `0xF3`, `0xF6`, and the supported
real-time statuses `0xF8`, `0xFA`–`0xFC`, `0xFE`, and `0xFF`. Supply exactly
the status-specific number of data bytes. Each data byte must be `0`–`127`;
SysEx and unsupported statuses are rejected.

## Soundboard

| Call | Description |
| --- | --- |
| `sound.play(basename, pitch)` | Play an AudioX sound by its basename, optionally changing pitch. Pitch defaults to `1.0` and must be `0.25`–`4.0`. |
| `sound.stop_all()` | Stop all sounds started by the soundboard. |

The sound name is a single preloaded basename, not a path; names containing
`..`, slashes, backslashes, controls, or NUL are rejected. The basename is
limited to 159 bytes. Upload and refresh the sound library in AudioX before
using a new sound; the trigger does not scan storage. See
[MIDI and soundboard](/lua/midi-and-soundboard).

## Effects and volume

| Call | Description |
| --- | --- |
| `effects.enable(id, enabled)` | Enable/disable an existing effect; `enabled` must be a boolean. |
| `effects.toggle(id)` | Toggle an existing effect's enabled state. |
| `effects.set(id, parameter, value)` | Set an existing effect parameter to a finite number in its actual range, not a normalized value. Unknown IDs/parameters and out-of-range values are runtime errors shown in the script list. |
| `volume.set(id, percent)` | Set an existing routing thing's gain to `0`–`100` percent: `0` is mute, `100` is unity (`gain = 1`). |
| `volume.adjust(id, delta)` | Adjust by `-100`–`100` percentage points, clamping the result to `0`–`100`. |

Effect targets must already exist and be connected into the intended audio path.
Find IDs and parameter metadata with `GET /api/effects`; use `fx_N` IDs, not
display labels. `effects.enable(id, false)` bypasses an ordinary effect.
For **Cut**, enabled means **muted** and disabled means **passing**.
`effects.toggle` flips that enabled flag, so the same function also toggles a
Cut mute.

`effects.set` changes a numeric parameter, not the effect type or its routes.
For example, with an existing Pitch effect whose ID is `fx_2`:

```lua
effects.enable("fx_2", true)
effects.set("fx_2", "semitones", 7)
effects.set("fx_2", "mix", 0.5)
```

Replace the example ID with your device's ID. The
[Effects guide](/guide/effects#effect-controls-and-defaults) lists all ranges,
and its [Lua section](/guide/effects#control-effects-from-lua) lists exact parameter
keys for every effect. For toggle and preset recipes, use the
[cookbook](/lua/cookbook#toggle-an-effect).

Use the exact routing thing `id` returned by `GET /api/audio/volumes`
(`{"volumes":[{"id":"…","name":"…","gain":0.8}]}`), not its display `name`.
IDs come from the current routing graph: for example `usb_gadget_out` or
`usb_gadget_in` when present, ALSA device IDs, and existing effect IDs such as
`fx_1`. Do not assume a generic `"master"`, `"input"` or `"output"` target.
Inspect the device's response before copying an ID into a script:

```sh
curl http://172.31.73.1/api/audio/volumes
```

For example, **only if `usb_gadget_in` appears in that response**:

```lua
volume.set("usb_gadget_in", 80)
volume.adjust("usb_gadget_in", -5)
```

Gain applies to a thing's outgoing routes. A final sink such as `usb_gadget_out`
does not attenuate its incoming signal using that gain; choose the source or
an intermediate processing node for audible level changes. See [Volume](/guide/volume).

The existing mixer allows gains up to `2`, but script controls deliberately
cap at unity. Percentages are not dB. Effect and volume actions are queued
runtime-only changes; they do not rewrite saved configuration and need no USB
keyboard host or MIDI controller. Route-changing automation is planned later.

## Utility functions

| Call | Description |
| --- | --- |
| `value_type(value)` | Return the Lua value type name (`"nil"`, `"boolean"`, `"number"`, `"string"`, or `"table"`). The global `type(text[, interval])` queues keyboard text instead. |
| `clamp(value, min, max)` | Limit a finite number to the inclusive range; `min` must not exceed `max`. |
| `map(value, inMin, inMax, outMin, outMax)` | Linearly map a finite number between ranges; input endpoints must differ. Values outside the input range extrapolate. |
| `round(value, decimals)` | Round a finite number to the nearest value at the requested decimal places (half away from zero); `decimals` defaults to `0` and must be an integer from `-6` to `6`. |
| `choose(array)` | Return one randomly selected item from a dense array indexed `1` through `1024`. |

### Selected Lua libraries

The safe base functions are `assert`, `error`, `ipairs`, `pairs`, `next`,
`select`, `tonumber`, and `tostring`. The type-inspection function is
`value_type`, because `type(text[, interval])` queues keyboard text.

Available `string` helpers: `byte`, `char`, `len`, `lower`, `upper`, `sub`,
`rep`, `reverse`, and `format`. String method syntax such as
`("Hi"):upper()` is supported. Pattern searching is not available.

Available `table` helpers: `concat`, `insert`, `remove`, `move`, and `unpack`.

Available `math` helpers: `abs`, `acos`, `asin`, `atan`, `ceil`, `cos`, `deg`,
`exp`, `floor`, `fmod`, `log`, `max`, `min`, `modf`, `rad`, `sin`, `sqrt`,
`tan`, `tointeger`, `type`, and `ult`, plus `pi`, `huge`, `mininteger`,
`maxinteger`, `random`, and `randomseed`. The deterministic pseudorandom
generator is seeded for each evaluation as `millis() XOR (channel << 24) XOR
(note << 8) XOR velocity`. The seed resets on each evaluation.
`math.randomseed(integer)` overrides it within that evaluation.
`math.random()` returns a number from 0 inclusive to 1 exclusive;
`math.random(n)` returns an integer from 1 through `n`;
`math.random(low, high)` returns an integer in the inclusive range.
Randomness is deterministic, not cryptographically secure.
Math functions require finite numeric arguments; constants such as `math.huge`
can be compared, but passing infinity to a math function is rejected.

Library work is bounded. Strings are limited to 4,096 bytes, table and string
spans to 1,024 items, and one evaluation to 65,536 units of cumulative library
work. `string.format` accepts at most 16 format fields with width and precision
up to 256.

## Example

```lua
state.count = (state.count or 0) + 1
local note = 60 + (event.note % 12)
local pitch = map(event.velocity, 1, 127, 0.75, 1.5)

log("play " .. tostring(note) .. " (#" .. state.count .. ")")
sound.play("hit.wav", pitch)
midi.note_on(0, note, event.velocity)
wait(150)
midi.note_off(0, note)
```

The event values and pitch are captured during evaluation. `wait(150)` delays
the queued MIDI note-off during playback. MIDI messages share an ordered,
nonblocking controller-output queue; waits begin after pending bytes have been
delivered.

## Device and failure requirements

These are firmware automation scripts, not browser scripts or always-on
automation. Only keyboard action plans require **USB Keyboard** enabled and a
connected USB host. Light/MIDI plans require the selected controller connected.
Sound, effects, volume, timing and logging need neither device. Sounds must be loaded
into AudioX before triggering.

A Lua evaluation error is atomic: no actions or state updates from that
evaluation are applied. A playback failure (for example, an unavailable
preloaded sound or controller output) cancels other pending scripts and resets
state. Already-delivered actions cannot be undone. AudioX best-effort releases
macro-generated MIDI notes and sustain when a sequence completes or is
canceled, including on disconnect.
