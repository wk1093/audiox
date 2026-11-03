# Getting started with Scripts

AudioX can run a short Lua program from web **Run**, MIDI, sound or controller event hooks. A
script can type on a USB keyboard, control its mapped pad's light, play an
AudioX sound, send MIDI to a connected controller, and make decisions using the
trigger and saved state. Scripts live on the standalone **Scripts** page, not MIDI.

## 1. Choose the devices your actions need

Open AudioX's **Config** page and enable **USB Keyboard**, then apply the
configuration **only if your action plan includes keyboard actions**. The keyboard
function is optional and disabled by default. Connect the Pi to a USB host;
the host should recognize it as a keyboard after it reconnects. Light and MIDI
actions instead need the selected MIDI controller connected. Sound, effects,
volume, logging and timing do not require either device.

## 2. Create a named script

Open **Scripts**, choose **New Script**, enter a name (1-63 UTF-8 bytes),
and replace the default callback with:

```lua callbacks
hook("run", function(event)
    state.runs = (state.runs or 0) + 1
    log("Run " .. state.runs)
end)

hook("device_connect", function(event)
    log("Connected " .. event.device)
end)
```

Save, then click **Run**. Hook calls choose subscriptions automatically.
Ordinary variables reset per event; only explicit `state` values persist in RAM.
Actions and state access belong inside handlers, not setup code.
See [callbacks and lifetime rules](/lua/events-and-state#register-callbacks-in-one-script).

Leave **Bind to a MIDI pad** unchecked for a web-only script. Choose
**Save Script**, then **Run** beside the saved entry. Run queues execution,
not completion. Reload Scripts to see runtime errors. Up to 16 scripts are
supported. Old sequences and shortcuts are [automatically converted](/guide/macros#existing-shortcuts-and-sequences).

To trigger from a controller too, check **Bind to a MIDI pad** and enter its
channel/note, or click **Learn MIDI Pad** and press it. Save the binding.
MIDI note-off runs the script only if note release is subscribed; it does not
automatically cancel the existing plan. Existing sound/action mappings
still run. **Edit** and **Remove** address the stable script ID: changing a
name or MIDI binding updates the same script. **New Script** clears the editor.

For a keyboard-and-controller example, enable the MIDI binding first, then use
these callbacks (`light()` without a binding reports a runtime error, even on Run):

```lua callbacks
local function play(event)
    light(12)
    tap("CTRL+A")
    wait(200)
    type("AudioX is ready!", 80)
    light(0)
end
hook("run", play)
hook("note_on", play)
```

The script requires both devices: it lights its configured pad, selects all text in the focused host application,
types a message, then turns the scripted light off. Click a text field on the
host before triggering it. Text is emitted as physical key presses, not pasted
from the clipboard. For a device-independent first test, use `log(event.source)` inside a run callback.
Keep idle and pressed LED velocities at their default `0` for web-only scripts
with no default light output.

## 3. Check the keyboard layout

`type()` supports printable US-ASCII characters, newline (`\n`), and tab
(`\t`). Set the host keyboard layout to US and turn Caps Lock off. Other layouts
or active host modifiers can change what appears. See
[keyboard behavior and lights](/lua/keyboard-and-lighting).

## Understand when a script runs

Each queued trigger starts one evaluation. Register subscriptions using `hook()`;
MIDI note press requires a `note_on` callback. `event.source` is `"web"`, `"midi"`,
`"audio"` or `"device"`. Web events use numeric configured channel/note and
velocity `127`; `event.pressed` is true. Lua runs briefly to build a bounded action
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
controller becomes unavailable—AudioX cancels pending scripts and resets state.
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
