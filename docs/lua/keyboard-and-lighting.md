# Keyboard and lighting

The script keyboard API sends physical HID key reports to the USB host. Enable
**USB Keyboard** in AudioX Config before relying on it. Only keyboard action
plans need this setting and a connected USB host. Light/MIDI plans need the
selected controller connected; sound/effects/volume plans need neither. These are
firmware-side macros triggered by web Run or MIDI—not browser automation or
always-on scripts. The keyboard does not paste text or target a specific
application; the host's currently focused app receives the keys.

## Chords and key names

Use `tap("CTRL+A")` to press a key combination briefly and release it.
`press("SHIFT+F13")` replaces the current held chord and keeps it held until
`release()`, another keyboard action replaces it, or the sequence ends. A
modifier-only hold such as `press("CTRL")` is valid.

Chord names are case-insensitive. Join keys with `+`; supported modifiers are
`CTRL`, `SHIFT`, `ALT`, and `GUI`. Key names are `A`–`Z`, `0`–`9`, `F1`–`F24`,
`ENTER`, `ESC`, `BACKSPACE`, `TAB`, `SPACE`, `DELETE`, `RIGHT`, `LEFT`, `DOWN`,
`UP`, `HOME`, `END`, `PAGEUP`, and `PAGEDOWN`. A chord can contain up to six
non-modifier keys; duplicate keys, empty names, and unsupported names are
rejected. Use `type()` for punctuation and text.

`tap()` and `type()` release a previous hold before acting; they do not inherit
modifiers. Thus, to hold Ctrl+A, write:

```lua
press("CTRL+A")
wait(200)
release()
```

Do not use `press("CTRL"); tap("A")` expecting Ctrl+A. Use the supported key
names and join simultaneous keys with `+`.

## Typing text

`type(text)` uses a 50 ms interval by default. `type(text, interval_ms)` accepts
an interval from **30 to 60,000 ms**. Each character is pressed for at least
30 ms and released before the next character, including repeated characters.
Supported text is printable US-ASCII, newline, and tab. A call may contain at
most 255 bytes, with a total of 256 characters across one evaluation.

The host must use a US keyboard layout with Caps Lock off for expected results.
Unicode, carriage returns, other control characters, and embedded NUL are
rejected.

```lua
type("AudioX\tready!\n", 70)
```

## Delays and held keys

`wait(ms)` queues a delay from `0` to `60,000` milliseconds. It is a playback
delay, not a Lua sleep. A later keyboard action waits for earlier reports to
be delivered and for taps or typed characters to release. Playback timing is a
minimum and can run longer due to device scheduling or host USB backpressure.

AudioX releases held keys when a sequence completes or is canceled. Macro
edits, cancellation and restarts reset state. Losing the USB keyboard session
cancels keyboard-dependent work, not all non-keyboard scripts. USB releases
during a disconnect are best effort.

## Pad lighting

`light(0)` turns off the mapped pad; `light(1)` through `light(127)` set its
velocity-style brightness. The scripted color is temporary: normal idle or
pressed lighting resumes when the sequence ends. Follow a light change with a
delay or another action if it needs to remain visible:

```lua
light(32)
wait(300)
light(0)
```

Lighting uses the configured pad's controller and does not change other pads.
Keep idle and pressed velocities at default `0` for web-only macros with no
default lighting. An explicit `light()` action requires an enabled MIDI binding
and the selected controller connected, even when the macro is run from the web.
Without that binding, `light()` reports an explicit runtime error rather than
choosing a default pad.
