# Lua script cookbook

Each example is a complete script for the **Scripts** page. Save it before
choosing **Run**. To add a MIDI trigger, bind the script and register the same
handler for `note_on`. Keyboard actions require USB Keyboard and a connected
host; MIDI and lighting require the selected controller; sound playback
requires a loaded clip. `light()` also requires a saved MIDI binding, including
for Web Run.

## Coarse microphone ducking

Replace the example IDs with routing IDs from your device. This uses the existing
one-second timer, after startup stabilization: it is coarse ducking, not an
audio-rate sidechain. Route the microphone through a separate branch so lowering
the music gain does not lower the detector itself. The timer deliberately does
not persist volume changes to flash. Disabling the script does not restore the
volume automatically; restore it with a Run callback before disabling.

```lua callbacks
hook("timer", function(event)
    local speaking = meters.get("usb_microphone", 0) > 0.03
    local target = speaking and 25 or 100
    if state.target ~= target then
        volume.set("music_bus", target)
        state.target = target
    end
end)

hook("run", function(event)
    volume.set("music_bus", 100)
    state.target = 100
end)
```

## Activity-based ducking

For faster ducking, replace the timer handler above with the following activity
handler, acknowledge the CPU warning, and keep the Run handler for restoring
volume. This still uses control-loop polling, not sample-accurate compression:

```lua callbacks
hook("device_active", "usb_microphone", function(event)
    local target = event.active and 25 or 100
    if state.target ~= target then
        volume.set("music_bus", target)
        state.target = target
    end
end)
```

## One CC controlling several effects

Bind this script to a MIDI channel and set its Note field to the CC number.
`effects.set` takes each parameter's actual units/range; scale the controller
value accordingly. These mix values use `0`–`1`.

```lua callbacks
hook("cc", function(event)
    local amount = event.value / 127
    effects.set("fx_1", "mix", amount)
    effects.set("fx_2", "mix", amount * 0.5)
end)
```

## One script, several callbacks

This script counts Web Run events and tracks completion of a named sound.
Setup variables reset per event; explicit `state` fields retain simple values.

```lua callbacks
local temporary = 0

hook("run", function(event)
    temporary = temporary + 1
    state.runs = (state.runs or 0) + 1
    log("Run " .. state.runs)
end)

hook("sound_finish", function(event)
    if event.sound == "intro.wav" then
        state.intros = (state.intros or 0) + 1
        log("Intros finished: " .. state.intros)
    end
end)
```

`temporary` starts at zero on every event. Callbacks for other events are not
invoked. Sound/controller hooks need no pad binding; add MIDI hooks only after
binding the script to the desired channel/note. A `wait()` queues action delays,
not a pause in Lua execution.

## Startup options and a custom button

These controls appear beside the saved script. The boot callback waits for the
healthy-startup interval; it does not run immediately during registration.
Replace the routing ID below with your actual source or processing node.

```lua callbacks
ui.option("enabled", {
    label = "Apply startup volume", type = "boolean", default = true
})
ui.option("level", {
    label = "Startup level (%)", type = "number", default = 50,
    min = 0, max = 100
})
ui.button("apply", "Apply volume now")

local function apply()
    if script_config.get("enabled", true) then
        volume.set("usb_gadget_in", script_config.get("level", 50))
    end
end

hook("boot", apply)
hook("ui", function(event)
    if event.key == "apply" then apply() end
end)
hook("config_change", function(event)
    log("Saved option: " .. event.key)
end)
```

Changing a control saves the option without editing source or parsing raw
configuration. `config_change` fires after that save. The Apply button queues
`ui`; it is independent of Web Run. No official-device write permission is
needed for these options or runtime volume changes.

## Keep a private startup record

```lua callbacks
hook("boot", function()
    local count = script_config.get("starts", 0) + 1
    script_config.set("starts", count)
    storage.write("startup.txt", "Startup " .. count .. "\n")
    log("Startup " .. count)
end)
```

The counter and file live in this script's isolated persistent directory,
not the official device configuration. Writes occur after successful
evaluation; they are separate actions, not a multi-file transaction. Write
occasionally rather than on every timer tick.

## React to Bluetooth changes

```lua callbacks
hook("bluetooth_change", function()
    local status = bluetooth.devices()
    for _, device in ipairs(status.devices) do
        if device.connected then log("Connected: " .. device.name) end
    end
end)
```

Device management scripts require confirmation on save, even for this
read-only example. To connect a saved device, use
`bluetooth.connect("AA:BB:CC:DD:EE:FF")` inside a callback, then wait for
`bluetooth_change`; do not edit pairing files or issue several requests at once.
To inspect diagnostics, use `system.logs("stderr", 20)` inside a callback.
`system.restart()` requests a whole-device reboot: avoid unconditional boot
reboots, which would create a restart loop.

## Count triggers

```lua callbacks
hook("run", function(event)
state.count = (state.count or 0) + 1
log("count = " .. tostring(state.count))
light((state.count % 8) * 16)
end)
```

The light cycles through eight levels. Persistent state only accepts simple
values; this counter stores one finite number.

## Show elapsed time between triggers

`millis()` is a monotonic snapshot at the note-on. The first event stores a
starting value; later events report the elapsed interval:

```lua callbacks
hook("run", function(event)
local now = millis()
if state.last_ms then
    local elapsed = now - state.last_ms
    log("elapsed ms: " .. tostring(elapsed))
end
state.last_ms = now
end)
```

The first event only initializes the counter. This compares trigger times;
adding `wait()` in a script will not advance `now`.

## Make a random choice

Choose and play one of several named soundboard items:

```lua callbacks
hook("run", function(event)
local sounds = { "bell.wav", "click.wav", "wood.wav" }
local selected = choose(sounds)
log("selected " .. selected)
sound.play(selected)
end)
```

`choose()` returns one array item using the bounded pseudorandom generator.
For explicit ranges, `math.random(1, 6)` can choose a number from 1 to 6.
Randomness is not cryptographic.

## Blink a pad

Queue a two-pulse pattern:

```lua callbacks
hook("run", function(event)
for pulse = 1, 2 do
    light(80)
    wait(150)
    light(0)
    wait(150)
end
end)
```

Loops execute during evaluation; `wait()` actions are queued into the playback
plan. Keep the total delay within the [limits](/lua/limits-and-troubleshooting).

## Reuse a function within one evaluation

Local functions help keep repeated actions readable. They do not persist
between note-ons:

```lua callbacks
hook("run", function(event)
local function flash(level, duration)
    light(level)
    wait(duration)
    light(0)
end

flash(24, 100)
flash(96, 250)
end)
```

## Type a velocity-dependent label

Choose a short response based on the input strength:

```lua callbacks
hook("run", function(event)
if event.velocity < 45 then
    type("soft", 60)
elseif event.velocity < 95 then
    type("medium", 60)
else
    type("strong", 60)
end
type("\n")
end)
```

Typing accepts only printable US-ASCII plus newline and tab. Confirm the
focused host application is safe to receive this text before triggering.

## Bluetooth media keys to computer keyboard

Save this as a new script with **MIDI binding disabled**, enable **USB Keyboard**
in Config, and connect AudioX's USB gadget port to the computer. Pair/connect your
Classic Bluetooth headphones or remote. This example is opt-in; it is not
automatically installed or enabled.

Defaults send actual USB consumer media keys, including play/pause, track skip,
and volume, so they are not tied to a focused player's keyboard shortcuts.
You can still set ordinary chords such as `SPACE` or `CTRL+RIGHT` in the script's
controls for application-specific behavior. Blank mappings ignore that key.
The optional device address restricts the bridge to one paired device.

```lua callbacks
local mappings = {
    {"play", "Play", "MEDIA_PLAY"},
    {"pause", "Pause", "MEDIA_PAUSE"},
    {"next", "Next track", "MEDIA_NEXT"},
    {"previous", "Previous track", "MEDIA_PREVIOUS"},
    {"stop", "Stop", "MEDIA_STOP"},
    {"rewind", "Rewind", "MEDIA_REWIND"},
    {"fast_forward", "Fast forward", "MEDIA_FAST_FORWARD"},
    {"volume_up", "Volume up", "MEDIA_VOLUME_UP"},
    {"volume_down", "Volume down", "MEDIA_VOLUME_DOWN"},
    {"mute", "Mute", "MEDIA_MUTE"},
    {"record", "Record", "MEDIA_RECORD"},
    {"eject", "Eject", "MEDIA_EJECT"}
}

ui.option("enabled", {label="Enabled", type="boolean", default=true})
ui.option("device", {label="Device MAC (blank = any)", type="string", default=""})
for _, mapping in ipairs(mappings) do
    ui.option(mapping[1], {label=mapping[2] .. " shortcut",
        type="string", default=mapping[3]})
end

hook("bluetooth_media_key", function(event)
    if not event.pressed or not script_config.get("enabled") then return end
    local device = script_config.get("device")
    if device ~= "" and string.upper(device) ~= event.device then return end
    local shortcut = script_config.get(event.key) or ""
    if shortcut ~= "" then tap(shortcut) end
end)
```

Only presses send a tap, so the corresponding Bluetooth release does not toggle
playback a second time. A held button may send repeated presses, depending on the
device. Taps include their own USB release, avoiding stuck keys if a Bluetooth
release is lost. Ordinary keyboard input goes to the focused computer application; media keys
are dispatched by the host OS/player. Not every player supports separate
Play/Pause, Record, or Eject; use `MEDIA_PLAY_PAUSE` for players that only support
toggle. Test safely. Invalid key names surface as script execution errors.

## Note with a timed release

```lua callbacks
hook("run", function(event)
local note = 36 + (event.note % 12)
midi.note_on(0, note, event.velocity)
wait(200)
midi.note_off(0, note)
end)
```

Always pair a raw MIDI note-on with its note-off. See
[MIDI and soundboard](/lua/midi-and-soundboard) for destination details.

## Toggle an effect

Replace `fx_1` with an existing effect ID from `GET /api/effects`. Each Run or
bound MIDI note-on flips its current enabled flag:

```lua callbacks
local function toggle(event)
effects.toggle("fx_1")
end
hook("run", toggle)
hook("note_on", toggle)
```

For ordinary effects this switches between processing and bypass. For **Cut**
it switches between muted and passing. No USB keyboard or MIDI controller is
required when using web Run.

## Alternate between two reverb presets

Create and route a Reverb effect first, then replace `fx_1` below with its ID.
The first trigger selects the wetter preset; the next selects the drier one:

```lua callbacks
hook("run", function(event)
state.wet = not state.wet
effects.enable("fx_1", true)
if state.wet then
    effects.set("fx_1", "wet_mix", 0.45)
    effects.set("fx_1", "feedback", 0.75)
else
    effects.set("fx_1", "wet_mix", 0.15)
    effects.set("fx_1", "feedback", 0.4)
end
end)
```

`state.wet` is the script's runtime memory, not a readback of the effect. Another
control can still change that effect between triggers. Both the state and
scripted parameter changes are runtime-only; these calls do not overwrite the
saved effect settings.

## Set pitch from pad velocity

For an existing Pitch effect, use the trigger strength to choose a shift from
minus twelve to plus twelve semitones:

```lua callbacks
local function pitch(event)
local semitones = round(map(event.velocity, 1, 127, -12, 12))
effects.enable("fx_2", true)
effects.set("fx_2", "semitones", semitones)
end
hook("run", pitch)
hook("note_on", pitch)
```

Replace `fx_2` with your Pitch effect's ID. Web Run supplies velocity 127 and
therefore selects +12; a MIDI pad supplies its actual note-on velocity.
Use actual parameter units and valid ranges, not a normalized value.
See [effect parameter keys](/guide/effects#control-effects-from-lua).
