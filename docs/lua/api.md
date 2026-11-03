# Lua API reference

Scripts use Lua 5.4 syntax in a restricted environment. The API is intentionally
small: there is no arbitrary filesystem, shell, network, dynamic loading, or unrestricted
device access. `_G`, `_VERSION`, metatables, pattern matching, module loading,
coroutines, `io`, `os`, `debug`, and `package` are not exposed. Each function
below is available globally unless shown under
`math`, `string`, `table`, `sound`, `midi`, `effects`, `volume`, `config`,
`settings`, `system`, `bluetooth`, `script_config`, `storage`, or `ui`.
Unless shown as complete callback scripts, examples below are handler-body
snippets: put them inside `hook("run", function(event) ... end)` or another
event callback, not at top level.

## Trigger and state

| Name | Description |
| --- | --- |
| `event.type` | `note_on`, `note_off`, `cc`, `sound_start`, `sound_finish`, `device_connect`, `device_disconnect`, `run`, `boot`, `timer`, `config_change`, `ui`, or `bluetooth_change`. |
| `event.source` | `"web"`, `"midi"`, `"audio"`, `"device"`, `"system"`, or `"bluetooth"`; all hooks use the same bounded queue. |
| `event.channel` | Triggering MIDI channel, `0`–`15`. |
| `event.note` | Triggering note, `0`–`127`. |
| `event.velocity` | MIDI velocity/value, `0`–`127`; zero for audio/device hooks. |
| `event.pressed` | True for press/start/connect and Web Run; false for release/CC/finish/disconnect. |
| `event.cc`, `event.value` | MIDI controller number and value; `value` mirrors velocity for notes. |
| `event.sound`, `event.device` | Sound basename or selected controller device path; empty when inapplicable. |
| `event.key` | Option/button key for `config_change` and `ui`; empty otherwise. |
| `state` | Persistent flat table for this mapping; see [events and state](/lua/events-and-state). |

## Callback registration

`hook(eventName, callback)` registers a script's event handlers. Event names
are the `event.type` values above; the second argument must be a Lua
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
is disabled and the Run API rejects execution.
See [callback examples and lifetime rules](/lua/events-and-state#register-callbacks-in-one-script).

### Startup and utility hooks

- `boot`: once after the main runtime has been ready for **60 seconds**, using
  the same healthy-startup interval as OS update recovery. Fires on ordinary
  boots too, without needing a trial update. A watchdog runtime restart starts
  a new interval; scripts saved after dispatch do not retroactively receive it.
- `timer`: once per second after startup stabilizes. Pending timer events for
  the same script coalesce; missed intervals are not replayed. This is a
  control-loop timer, not an audio/sample-accurate clock.
- `bluetooth_change`: Bluetooth adapter, discovery, pairing, device, or
  connection status changed. Read `bluetooth.devices()` for the latest snapshot.
- `config_change`: a WebUI option was persisted; `event.key` names it and
  `event.value` is its typed value. Script-side `script_config.set()` does not
  retrigger this hook, avoiding feedback loops.
- `ui`: a registered WebUI button was pressed; `event.key` identifies it and
  `event.value` is nil.

No MIDI binding is needed for these hooks. All use the existing bounded action
queue and evaluation limits; successful HTTP requests confirm queuing, not
callback completion.

## Script-owned configuration and WebUI

| Call | Description |
| --- | --- |
| `script_config.get(key[, default])` | Read this script's saved scalar option; missing keys return the supplied default or nil. |
| `script_config.set(key, value)` | Queue a persistent boolean, finite number, or string; nil removes the key. |
| `ui.option(key, descriptor)` | Declare a native WebUI option at top level. Descriptor fields: `label`, `type`, `default`, optional numeric `min` and `max`. Types: `"boolean"`, `"number"`, `"string"`. |
| `ui.button(key, label)` | Declare a native WebUI button at top level; handled by `hook("ui", ...)`. |

Up to 16 controls and 32 saved scalar keys per script. Keys are bounded to 31
bytes; strings to 127 bytes. Declared options validate both Lua and HTTP writes
against their type/range. Defaults are used when no saved value exists and are
not automatically written to flash. `script_config.get(key)` uses a declared
option's default when unset; an explicit fallback overrides that default.
Register declarations unconditionally,
alongside hooks; read/write saved options inside callbacks.

Controls appear beside the saved script on **Scripts**, not arbitrary pages.
Labels and values are rendered as text; scripts cannot inject HTML, JavaScript,
CSS, or external resources. Editing an option persists it before queuing
`config_change`. These settings do not require official-device write permission.
Script identity, not display name or MIDI binding, owns the stored data.

## Script-owned files

| Call | Description |
| --- | --- |
| `storage.list()` | List this script's custom file basenames. |
| `storage.read(name)` | Read a custom file; nil if missing. |
| `storage.write(name, data)` | Queue an atomic, flushed custom-file replacement. |
| `storage.remove(name)` | Queue deletion of a custom file. |

Custom files can contain arbitrary bytes, including NUL. Limits are **32 files**,
**4,096 bytes per file**, and **65,536 bytes total per script**, including its
reserved options store. Basenames are at most 63 ASCII bytes (`A–Z`, `a–z`,
digits, `_`, `-`, `.`); leading dots and `..` are rejected. Reads/listing share
a 32-call per-event limit and see earlier queued file changes from that event.
Option reads also see earlier queued option changes.

Storage is confined to `/audiox/scripts/<stable-script-id>/` on the persistent
partition. Only flat, validated basenames are accepted: no absolute paths,
directory traversal, symlinks, hard links, or access to another script.
The options store is reserved and cannot be accessed through the file API.
**Official device configuration and Bluetooth credentials are never exposed
through this directory**; use `settings`, `effects`, `volume`, `bluetooth`,
or the legacy allowlisted `config` API instead.

Custom files and options survive reboot and source/name edits. Removing a
script retains its data; newly allocated script IDs do not reuse stored
directories. The standard configuration backup currently includes script
definitions, **not this script-owned storage**. Export important data separately.
Writes share the per-event action/payload budgets and occur only after successful
evaluation. An execution failure does not undo earlier actions. Avoid writing
on every timer, CC, or sound event to limit flash wear and control-loop stalls.

## Structured device settings

`settings.get()` returns an active-configuration table. `settings.set(key, value)`
queues a validated update of one field, preserving the other supported fields:

| Key | Value |
| --- | --- |
| `usb_sample_rate` | Integer 8000–192000; descriptors only, not DSP engine retuning. |
| `usb_playback_channels`, `usb_capture_channels` | Integer 1–16. |
| `usb_sample_size` | Integer 1–4 bytes. |
| `keyboard_macros` | Boolean. |
| `soundboard_mode` | `"play"` or `"hold"`. |
| `framebuffer_view` | `"meters"` or `"soundboard"`. |
| `midi_passthrough` | `"off"`, `"unmapped"`, or `"all"`. |

Writes require **Allow device management/configuration writes**. Unlike the
Config page's staged USB editor, these update active persistent configuration.
USB descriptor changes need a gadget reload or reboot; they do not apply
silently during the callback. Runtime-only effects/volume actions remain
preferable for live control.

## System and Bluetooth utilities

| Call | Description |
| --- | --- |
| `system.restart()` | Queue a flushed whole-device reboot; requires device-management permission. |
| `system.logs([source[, limit]])` | Read recent `"stdout"` (default) or `"stderr"` lines as text; default 32, maximum 128 lines, bounded output. |
| `bluetooth.devices()` | Snapshot with `ready`, `scanning`, `pairing`, `message`, and a `devices` array. Entries contain `address`, `name`, `paired`, `auto_connect`, `connected`, `connecting`, `audio_source`, and `audio_sink`; no pairing keys. |
| `bluetooth.scan()` | Queue discovery. |
| `bluetooth.pair(address)` | Queue pairing of a discovered device. |
| `bluetooth.connect(address)` | Queue connection. |
| `bluetooth.disconnect([address])` | Queue disconnection; omitted address disconnects all audio links. |
| `bluetooth.unpair(address)` | Queue removal of pairing. |
| `bluetooth.auto_connect(address, enabled)` | Persist a boolean auto-connect preference. |

Bluetooth mutations require device-management permission and use the existing
worker request APIs. Busy/unavailable requests fail explicitly; request
acceptance does not imply connection/pairing completion. Wait for
`bluetooth_change` and inspect the snapshot rather than issuing several
concurrent requests. Logs/settings/Bluetooth reads are callback-only and
bounded; unavailable resources raise Lua errors.

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

The editor warns before saving scripts referencing device-management APIs, including read-only
scripts and comments. **Allow device management/configuration writes** explicitly requests
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

The mixer allows gains up to `2`, but script volume controls cap at unity.
Percentages are not dB. Effect and volume actions are runtime-only; they do not
rewrite saved configuration and need no USB keyboard host or MIDI controller.
The Lua API does not change routing.

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
