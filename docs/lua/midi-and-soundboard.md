# MIDI output and soundboard

Scripts can trigger preloaded AudioX soundboard files and send MIDI back to the
connected controller. These are separate from MIDI input mapping: a firmware
automation macro runs on web Run or a bound note-on, then plays its queued plan.
MIDI and lighting plans require the selected MIDI controller connected. Only
keyboard action plans need USB Keyboard enabled and a connected USB host.
Sound-only plans need neither; preload sounds before running. These are bounded
event-triggered macros, not independent always-on automation.

## Play a sound

Use a sound's basename, with no directory:

```lua
sound.play("bell.wav")
sound.play("low.wav", 0.8)
```

The optional pitch ratio ranges from `0.25` to `4.0`; its default is `1.0`.
Names are constrained to a single preloaded basename of 1–159 bytes. Names
with `..`, path separators, controls, or NUL are rejected; scripts cannot
choose arbitrary paths. Use AudioX's normal Upload/Refresh controls to load a
WAV before triggering the script. The device does not scan storage on each
trigger. A missing sound fails explicitly during playback.
`sound.stop_all()` stops soundboard playback:

```lua
sound.play("loop.wav")
wait(1200)
sound.stop_all()
```

Use only names that exist in the device's loaded sound library.

## Send MIDI

The destination is the connected MIDI controller, **not the USB host**.
`midi.send()` accepts a complete short MIDI message; status-specific message
lengths and 7-bit data-byte values are checked. Prefer the helpers for common
channel messages:

```lua
midi.cc(0, 1, event.velocity)
midi.program(0, 12)
midi.note_on(0, 60, 100)
wait(250)
midi.note_off(0, 60)
```

Channels are zero-based, `0`–`15`; notes, velocity, controller, program, and
data bytes use MIDI's `0`–`127` range. A note-on velocity of zero has the
standard MIDI meaning of note-off. Note-off velocity defaults to zero.

Raw `midi.note_on()` sends a note but does not automatically pair it with a
note-off in the script. Pair notes and allow enough time for the controller to
receive both messages. Messages share an ordered, nonblocking controller
output queue with lighting, including when a write completes in multiple
pieces. A `wait()` begins after pending bytes have been delivered.
AudioX bounds controller output and does not block the audio loop while a
device is busy. Macro-generated notes and sustain are best-effort released when
a sequence completes or is canceled, including on disconnect.

```lua
local note = 48 + (event.note % 24)
midi.note_on(1, note, event.velocity)
wait(300)
midi.note_off(1, note)
```

## Soundboard and input mappings

A Lua mapping can coexist with other configured MIDI behavior for a trigger.
The script's MIDI output is separate from the incoming note and does not
re-route arbitrary host messages. For full mapping and soundboard setup, see
the [MIDI guide](/guide/midi) and [Soundboard guide](/guide/soundboard).

A playback failure (for example, an unavailable loaded sound or controller
output) cancels other pending macros and resets state. Actions already
delivered cannot be undone.
