# Keyboard and lighting

Keyboard actions send physical HID reports to the connected USB host. Enable
**USB Keyboard** in Config; only scripts with keyboard actions require it. These
are event-triggered firmware actions, not browser automation. The host's
focused application receives the keys; AudioX does not paste text or choose a
target application. MIDI/lighting plans need the selected controller, while
sound, effect, and volume plans need neither device.

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

## Media keys

`tap("MEDIA_PLAY_PAUSE")` sends a real USB Consumer Control report, not a
keyboard shortcut. `press("MEDIA_VOLUME_UP")` holds a media key; `release()`
or sequence completion releases it. Names are case-insensitive:

| Key | Function |
| --- | --- |
| `MEDIA_PLAY`, `MEDIA_PAUSE`, `MEDIA_PLAY_PAUSE` | Separate play/pause or toggle |
| `MEDIA_NEXT`, `MEDIA_PREVIOUS`, `MEDIA_STOP` | Track skip and stop |
| `MEDIA_REWIND`, `MEDIA_FAST_FORWARD` | Seek |
| `MEDIA_VOLUME_UP`, `MEDIA_VOLUME_DOWN`, `MEDIA_MUTE` | Host volume |
| `MEDIA_RECORD`, `MEDIA_EJECT` | Record/eject where supported |

Use one media key per call; media keys cannot be combined with modifiers or
ordinary keys using `+`. Taps release automatically after at least 30 ms.
Switching between a held media key and an ordinary chord releases the previous
report first. Both share the bounded playback queue and cancellation rules.

**USB Keyboard** exposes keyboard and Consumer Control collections through one
HID interface (`/dev/hidg0`). Report ID 1 carries eight keyboard bytes; report
ID 2 carries two media bytes. Sharing the interface avoids exhausting the Pi's
USB gadget endpoints when audio, networking, and MIDI are also enabled. This is
a report-protocol keyboard, not a BIOS/UEFI boot-protocol keyboard.
Apply the USB configuration or reboot after updating to enumerate it. Media keys
are dispatched by the computer's OS/player, which may not support every usage.
The [Bluetooth bridge](./cookbook#bluetooth-media-keys-to-computer-keyboard) uses
these by default, with configurable ordinary-chord overrides.

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

AudioX releases held keys when a sequence completes or is canceled. Script
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
Keep idle and pressed velocities at default `0` for web-only scripts with no
default lighting. An explicit `light()` action requires an enabled MIDI binding
and the selected controller connected, even when the script is run from the web.
Without that binding, `light()` reports an explicit runtime error rather than
choosing a default pad.
