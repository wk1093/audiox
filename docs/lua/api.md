# Lua API reference

Scripts use Lua 5.4 syntax in a restricted environment. The API is intentionally
small: there is no filesystem, shell, network, dynamic loading, or unrestricted
device access. `_G`, `_VERSION`, metatables, pattern matching, module loading,
coroutines, `io`, `os`, `debug`, and `package` are not exposed. Each function
below is available globally unless shown under
`math`, `string`, `table`, `sound`, `midi`, `effects`, or `volume`.

## Trigger and state

| Name | Description |
| --- | --- |
| `event.source` | `"web"` or `"midi"`; web Run and MIDI note-on use the same queue. |
| `event.channel` | Triggering MIDI channel, `0`–`15`. |
| `event.note` | Triggering note, `0`–`127`. |
| `event.velocity` | Triggering note-on velocity, `1`–`127`. |
| `event.pressed` | Always `true`; note-off does not execute Lua. Web uses configured numeric channel/note and velocity `127`. |
| `state` | Persistent flat table for this mapping; see [events and state](/lua/events-and-state). |

## Keyboard, timing, and logging

| Call | Description |
| --- | --- |
| `tap(keys)` | Tap a chord and release it. Example: `tap("CTRL+A")`. |
| `press(keys)` | Replace the currently held chord. Modifier-only chords are allowed. |
| `release()` | Release all keys held by the script. |
| `type(text, interval_ms)` | Type supported US-ASCII text. The interval defaults to 50 ms. |
| `wait(ms)` | Add a playback delay, `0`–`60000` ms. |
| `light(level)` | Temporarily set the bound pad light, `0`–`127`. Requires an enabled MIDI binding and the selected connected controller, including for web Run; no binding is an explicit runtime error. |
| `millis()` | Monotonic milliseconds captured at the original note-on. |
| `epoch()` | Unix seconds captured at the original note-on; the device clock may be unset. |
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
| `effects.set(id, parameter, value)` | Set an existing effect parameter to a finite number in its actual range, not a normalized value. Unknown IDs/parameters and out-of-range values are runtime errors shown in the macro list. |
| `volume.set(id, percent)` | Set an existing routing thing's gain to `0`–`100` percent: `0` is mute, `100` is unity (`gain = 1`). |
| `volume.adjust(id, delta)` | Adjust by `-100`–`100` percentage points, clamping the result to `0`–`100`. |

Use the exact routing thing `id` returned by `GET /api/audio/volumes`
(`{"volumes":[{"id":"…","name":"…","gain":0.8}]}`), not its display `name`.
IDs come from the current routing graph: for example `usb_gadget_out` or
`usb_gadget_in` when present, ALSA device IDs, and existing effect IDs such as
`fx_1`. Do not assume a generic `"master"`, `"input"` or `"output"` target.
Inspect the device's response before copying an ID into a script:

```sh
curl http://172.31.73.1/api/audio/volumes
```

For example, **only if `usb_gadget_out` appears in that response**:

```lua
volume.set("usb_gadget_out", 80)
volume.adjust("usb_gadget_out", -5)
```

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

These are firmware automation macros, not browser scripts or always-on
automation. Only keyboard action plans require **USB Keyboard** enabled and a
connected USB host. Light/MIDI plans require the selected controller connected.
Sound, effects, volume, timing and logging need neither device. Sounds must be loaded
into AudioX before triggering.

A Lua evaluation error is atomic: no actions or state updates from that
evaluation are applied. A playback failure (for example, an unavailable
preloaded sound or controller output) cancels other pending macros and resets
state. Already-delivered actions cannot be undone. AudioX best-effort releases
macro-generated MIDI notes and sustain when a sequence completes or is
canceled, including on disconnect.
