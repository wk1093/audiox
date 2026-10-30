# Limits and troubleshooting

Lua is intended for short, bounded controller actions, not general-purpose
programs or long-running tasks. Each web Run or MIDI note-on builds one plan, which
AudioX replays later. The following limits apply to an evaluation:

| Resource | Limit |
| --- | ---: |
| Named macros | 16 |
| Name | 63 UTF-8 bytes |
| Source text | 2,047 bytes |
| Queued actions | 64 |
| Total waits and typing duration | 60,000 ms |
| Instructions | 10,000 |
| Lua memory | 256 KiB |
| Type text | 255 bytes per call; 256 characters total |
| Persistent state | 32 scalar fields |
| State key | 31 bytes |
| State string value | 127 bytes |
| String value | 4,096 bytes |
| Table or string span | 1,024 items |
| Cumulative library work | 65,536 units |
| `string.format` fields | 16; width and precision up to 256 |

The engine also limits queued work to one evaluation per 10 ms, allows at most
16 macros and 16 pending triggers, and uses a 65-second playback timeout.
These are safety bounds, not timing guarantees.

## An evaluation failed

Check the script's reported error, then verify:

- Every keyboard chord uses supported key names and valid `+`-separated keys.
- `wait()` receives an integer from `0` to `60000`.
- `type()` receives allowed characters and an interval from `30` to `60000`.
- MIDI and light values are valid integers in their documented ranges.
- The script stays within source, action, duration, instruction, and memory limits.
- `state` contains no more than 32 scalar values.

Failed evaluations are transactional: queued actions and state updates are
discarded, including actions created before the error.

## No keyboard input appears

1. Enable **USB Keyboard** in AudioX Config and apply the configuration.
2. Confirm that the Pi is connected to the host as a USB device.
3. Focus an editable field in the intended host application.
4. Check the host layout and Caps Lock for `type()`.
5. Try one small `tap("A")` test.

Keyboard actions can be canceled by editing a mapping, disconnecting, disabling
the feature, or restarting AudioX. Disconnect-time key releases are best effort.

## Text differs from the script

`type()` simulates keys for a US layout; it does not transmit Unicode text.
Use a US host layout with Caps Lock off. Unicode, carriage return, other
control characters, and embedded NUL are not supported.

## A light changes briefly or not at all

`light()` temporarily changes only the mapped pad. Normal controller lighting
resumes at sequence completion; place `wait()` or another action after a light
call to keep it visible. Check that the selected controller is connected and
**Bind to a MIDI pad** is enabled and saved. `light()` without a MIDI binding
reports an explicit runtime error, including for web Run; a configured note
number alone is not a binding.

## MIDI or sound output is missing

MIDI script output is directed to the connected MIDI controller, never
automatically to the USB host. Ensure the intended controller is connected and
use zero-based channel numbers. For raw note output, send a matching note-off.
For soundboard playback, check that the basename identifies an available
preloaded sound; use AudioX Upload/Refresh before triggering. The device does
not scan storage on each trigger. Device disconnection or bounded output
backpressure can prevent queued output from completing. Light/MIDI plans need
the selected connected controller, but not USB Keyboard or a USB host.
Only keyboard action plans require that setting and host. Sound, effects and
volume plans need neither device.

## Counters or clocks seem to reset

`state` resets on macro edits, cancellation/failure, or AudioX restart.
USB loss and keyboard disable do not globally reset non-keyboard scripts.
`millis()` and `epoch()` are frozen to the original trigger while a plan waits
in the queue; evaluate a later note-on to obtain a new snapshot. The device's
wall clock may be unset, making `epoch()` unsuitable for absolute timestamps
until system time is configured.

A playback failure (such as a missing loaded sound) cancels pending macros and
resets state. Evaluation failures are atomic, but playback failures cannot undo
actions already delivered.

## Web Run or a device control fails

Run queues a saved macro by its stable ID; a successful response is not a
playback-completion acknowledgment. Non-200 responses appear as errors; reload
the macro list for later runtime errors. A failed save keeps the editor contents.
Check the 16-macro limit and count name/source lengths in UTF-8 bytes, not
characters.

Use the exact routing thing ID returned by `/api/audio/volumes` for volume
actions, not a display name or generic `"master"`. Script volume percentages
stop at unity (`100%`); adjustments are percentage points clamped to `0`–`100`.
Effect values use actual parameter ranges, not normalized `0`–`1` values;
out-of-range values are runtime errors. These controls are runtime-only and
do not save configuration. Routing automation is not yet supported.
