# Lua scripting

The [Scripts system](/guide/macros) runs Lua callbacks on AudioX when triggered
from Web Run or subscribed MIDI, sound, and controller events. Scripts do not
run in this documentation site.

Scripts combine Lua control flow and bounded standard-library helpers with
trigger data and per-script runtime state. Device functions queue keyboard,
controller MIDI/lighting, sound, effect, and volume actions.

## Read in order

1. [Getting started](./getting-started) - create and run your first named script.
2. [Events and state](./events-and-state) - trigger sources, counters, clocks,
   toggles, and the difference between evaluation and playback.
3. [API reference](./api) - complete supported functions, arguments, and units.
4. [Keyboard and lighting](./keyboard-and-lighting) - holds, text layout,
   automatic release, and bound-pad lights.
5. [MIDI and soundboard](./midi-and-soundboard) - controller output and preloaded
   clip actions.
6. [Cookbook](./cookbook) - small copyable recipes.
7. [Limits and troubleshooting](./limits-and-troubleshooting) - resource bounds,
   errors, device requirements, and unavailable libraries.

For general device setup rather than scripting, use the
[device getting-started guide](/guide/getting-started).
