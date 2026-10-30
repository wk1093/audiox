# Lua scripting

Lua scripts are one type of [named macro](/guide/macros). They run on AudioX
from a saved WebUI Run button or a MIDI note-on binding, not in this website.

You can combine functions, conditionals, loops, arithmetic, bounded string/table/
math helpers, trigger information, and per-macro runtime state. Device functions
queue actions for keyboard output, controller lights/MIDI, preloaded sounds,
effects, and volume.

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
