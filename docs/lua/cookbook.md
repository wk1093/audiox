# Lua script cookbook

These complete examples assume each macro's state starts empty. Name and edit a
script on the **Macros** page, save it, and click **Run** or bind a MIDI note-on.
They run on AudioX firmware. Only keyboard action plans need USB Keyboard
enabled and a connected USB host; light/MIDI plans need the selected controller
connected. Preload any referenced sound before triggering. For web-only macros
with no default light output, leave both LED velocities at `0`. Recipes using
`light()` require an enabled MIDI binding and the selected connected controller,
even on web Run; otherwise `light()` reports a runtime error.

## Count triggers

```lua
state.count = (state.count or 0) + 1
log("count = " .. tostring(state.count))
light((state.count % 8) * 16)
```

The light cycles through eight levels. Persistent state only accepts simple
values; this counter stores one finite number.

## Show elapsed time between triggers

`millis()` is a monotonic snapshot at the note-on. The first event stores a
starting value; later events report the elapsed interval:

```lua
local now = millis()
if state.last_ms then
    local elapsed = now - state.last_ms
    log("elapsed ms: " .. tostring(elapsed))
end
state.last_ms = now
```

The first event only initializes the counter. This compares trigger times;
adding `wait()` in a script will not advance `now`.

## Make a random choice

Choose and play one of several named soundboard items:

```lua
local sounds = { "bell.wav", "click.wav", "wood.wav" }
local selected = choose(sounds)
log("selected " .. selected)
sound.play(selected)
```

`choose()` returns one array item using the bounded pseudorandom generator.
For explicit ranges, `math.random(1, 6)` can choose a number from 1 to 6.
Randomness is not cryptographic.

## Blink a pad

Queue a two-pulse pattern:

```lua
for pulse = 1, 2 do
    light(80)
    wait(150)
    light(0)
    wait(150)
end
```

Loops execute during evaluation; `wait()` actions are queued into the playback
plan. Keep the total delay within the [limits](/lua/limits-and-troubleshooting).

## Reuse a function within one evaluation

Local functions help keep repeated actions readable. They do not persist
between note-ons:

```lua
local function flash(level, duration)
    light(level)
    wait(duration)
    light(0)
end

flash(24, 100)
flash(96, 250)
```

## Type a velocity-dependent label

Choose a short response based on the input strength:

```lua
if event.velocity < 45 then
    type("soft", 60)
elseif event.velocity < 95 then
    type("medium", 60)
else
    type("strong", 60)
end
type("\n")
```

Typing accepts only printable US-ASCII plus newline and tab. Confirm the
focused host application is safe to receive this text before triggering.

## Note with a timed release

```lua
local note = 36 + (event.note % 12)
midi.note_on(0, note, event.velocity)
wait(200)
midi.note_off(0, note)
```

Always pair a raw MIDI note-on with its note-off. See
[MIDI and soundboard](/lua/midi-and-soundboard) for destination details.
