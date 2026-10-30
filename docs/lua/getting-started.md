# Getting started with named automation macros

AudioX can run a short Lua program from a web **Run** button or MIDI note-on. A
script can type on a USB keyboard, control its mapped pad's light, play an
AudioX sound, send MIDI to a connected controller, and make decisions using the
trigger and saved state. Macros live on the standalone **Macros** page, not MIDI.

## 1. Choose the devices your actions need

Open AudioX's **Config** page and enable **USB Keyboard**, then apply the
configuration **only if your action plan includes keyboard actions**. The keyboard
function is optional and disabled by default. Connect the Pi to a USB host;
the host should recognize it as a keyboard after it reconnects. Light and MIDI
actions instead need the selected MIDI controller connected. Sound, effects,
volume, logging and timing do not require either device.

## 2. Create a named script

Open **Macros**, enter a name (1–63 UTF-8 bytes), choose **Lua sequence** and
enter a script. Leave **Bind to a MIDI pad** unchecked for a web-only macro.
Save, then click **Run** beside the saved macro. Run queues execution; it does
not confirm that playback has completed. Reload Macros to see runtime errors.
Up to 16 macros are supported.

To trigger from a controller too, check **Bind to a MIDI pad** and enter its
channel/note, or click **Learn MIDI Pad** and press it. Save the binding.
MIDI note-off does not run or cancel the script. Existing sound/action mappings
still run. **Edit** and **Remove** address the stable macro ID: changing a
name or MIDI binding updates the same macro. **New Macro** clears the editor.

For a keyboard-and-controller example, enable the MIDI binding first, then use
this sequence (`light()` without a binding reports a runtime error, even on Run):

```lua
light(12)
tap("CTRL+A")
wait(200)
type("AudioX is ready!", 80)
light(0)
```

The script requires both devices: it lights its configured pad, selects all text in the focused host application,
types a message, then turns the scripted light off. Click a text field on the
host before triggering it. Text is emitted as physical key presses, not pasted
from the clipboard. For a device-independent first test, use `log(event.source)`.
Keep idle and pressed LED velocities at their default `0` for web-only macros
with no default light output.

## 3. Check the keyboard layout

`type()` supports printable US-ASCII characters, newline (`\n`), and tab
(`\t`). Set the host keyboard layout to US and turn Caps Lock off. Other layouts
or active host modifiers can change what appears. See
[keyboard behavior and lights](/lua/keyboard-and-lighting).

## Understand when a script runs

Each Run or MIDI note-on starts one evaluation. `event.source` is `"web"` or
`"midi"`. Web events use numeric configured channel/note and velocity `127`;
`event.pressed` is true. Lua runs briefly to build a bounded action
plan; its actions are then played back cooperatively. A `wait()` delays playback
but does not pause or resume Lua code. Every event value and clock helper is a
snapshot from the original trigger, even if playback is delayed behind another
sequence. Sound files must already be loaded into AudioX; playback does not scan
the storage on each trigger. See [events and state](/lua/events-and-state).

## Learn and test safely

Begin with a small script and trigger it from the intended pad. If an evaluation
fails, AudioX reports a script error and commits neither its actions nor its
state changes. A successful script may still be canceled if the mapping is
edited, required devices disconnect, or AudioX restarts. USB loss or keyboard
disable does not globally reset scripts that do not use keyboard actions.
If playback fails—for example, because a referenced sound is not loaded or the
controller becomes unavailable—AudioX cancels pending macros and resets state.
Actions already delivered cannot be undone. Evaluations that fail before
playback remain atomic.

Read the [API reference](/lua/api) and [limits and troubleshooting](/lua/limits-and-troubleshooting)
before building longer sequences.

## Control effects and volumes

Scripts can enable, bypass, or toggle existing effects and change their
parameters with `effects.enable`, `effects.toggle`, and `effects.set`.
For examples, see [effect recipes](/lua/cookbook#toggle-an-effect) and the
[Effects guide](/guide/effects#control-effects-from-lua).

Effect and volume controls are runtime-only; they do not save persistent
configuration. Use existing effect IDs and routing thing IDs, not display
names or invented targets like `"master"`. See the [API reference](/lua/api#effects-and-volume).
Route-changing automation is planned later.
