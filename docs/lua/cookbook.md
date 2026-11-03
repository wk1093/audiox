# Lua script cookbook

Each example is a complete script for the **Scripts** page. Save it before
choosing **Run**. To add a MIDI trigger, bind the script and register the same
handler for `note_on`. Keyboard actions require USB Keyboard and a connected
host; MIDI and lighting require the selected controller; sound playback
requires a loaded clip. `light()` also requires a saved MIDI binding, including
for Web Run.

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
