---
title: Effects
description: Create AudioX audio effects, tune their parameters, and map MIDI controls.
---

# Effects

AudioX effects are processing nodes in the Routing graph. Add one from the
graph's Add menu, connect its input and output channels, then use the controls
on the effect node to adjust its sound. Open **Effects** to enable or bypass
effects, assign MIDI controls, set LED values, or delete an effect.

WebUI effect changes take effect as you make them and are written to AudioX's
persistent effect configuration. Lua effect changes are runtime-only.
There is no separate Save button for the parameter sliders. A newly created effect receives an `fx_N` ID; use that ID
when identifying the same effect in Routing, Effects, or automation.

## Add, connect, and edit an effect

1. On **Routing**, open the graph Add menu and choose the effect type.
2. Connect the incoming route to the effect's input channel and the outgoing
   route from its matching output channel. Effects preserve channel positions;
   connect channel 0 to channel 0, channel 1 to channel 1, and so on.
3. Adjust the parameter sliders displayed on the effect node. Changes are
   applied after a short debounce and saved automatically.
4. On **Effects**, use **Bypass**/**Enable** for ordinary effects. A Cut node
   uses **Mute**/**Unmute** instead. This panel also contains MIDI mapping,
   LED color, and delete controls.

Deleting an effect from the Effects page removes its saved effect state and its
node from Routing. Check the graph and save the route changes you intend to
keep.

## Effect controls and defaults

The controls below are the available parameter ranges and factory defaults.
Values are not decibels unless the label says `dB`. WebUI controls are bounded
to these ranges; Lua rejects out-of-range values. Slider step sizes are interface
increments; the backend accepts the listed ranges.

| Effect | Control | Range | Default |
| --- | --- | ---: | ---: |
| **Gain** | Level | 0–4 | 1 |
| **Distortion** | Input Gain | 0–4 | 1 |
|  | Drive | 0–8 | 4 |
|  | Clip | 0.05–1 | 0.35 |
|  | Output Gain | 0–4 | 1 |
| **Pitch** | Input Gain | 0–4 | 1 |
|  | Semitones | −12–12 | 0 |
|  | Mix | 0–1 | 1 |
|  | Output Gain | 0–4 | 1 |
|  | Noise Floor | 0.0001–0.02 | 0.001 |
|  | Wet LPF Hz | 1,000–18,000 | 12,000 |
| **Reverb** | Input Gain | 0–4 | 1 |
|  | Feedback | 0.05–0.95 | 0.64 |
|  | Damping | 0–1 | 0.48 |
|  | Wet Mix | 0–1 | 0.26 |
| **Noise Gate** | Threshold dB | −80–0 dB | −62 dB |
|  | Attack ms | 0.2–100 ms | 2 ms |
|  | Release ms | 5–600 ms | 160 ms |
|  | Range dB | 0–80 dB | 30 dB |
| **Cut** | Channels | 1–16, whole numbers | 2 |

Cut's **Channels** setting changes how many input and output sockets the node
has. If the count changes, reconnect routes that refer to channels that no
longer exist. A newly created Cut is in its passing (not muted) state.

## MIDI controls and effect LEDs

Connect the MIDI controller you want to use, then open **Effects**:

- In a parameter row, choose **Map** and move the desired controller. AudioX
  captures the next incoming CC number and maps it to that parameter. The CC's
  0–127 value spans the parameter's full range; for example, CC 0 selects the
  minimum and CC 127 the maximum.
- Choose **Clear** in that row to remove its current CC mapping.
- In **Bypass Toggle** (or **Mute Toggle** for Cut), choose **Map** and press a
  MIDI note. **Clear** removes that note mapping.
- Set the two LED velocity values (0–127) for the enabled and bypassed states,
  then choose **Save**. For Cut, the labels are muted and passing. **Default**
  returns the effect to the global LED values.

Mappings are selected by MIDI CC or note number; the Effects page does not
offer a per-channel filter. As with other MIDI features, use the currently
selected controller. Mapping and LED edits are persisted as you apply them.
See [MIDI](./midi.md) for controller setup and
[Macros](./macros.md) for automation.

## Control effects from Lua

**Yes: Lua macros can toggle effects, explicitly enable/bypass them, and change
their parameters.** A web-triggered effect script does not need a MIDI controller
or the USB keyboard function.

Create and connect the effect in Routing first. Find its exact `fx_N` ID in
`GET /api/effects`, then use it in a Lua sequence on **Macros**:

```lua
-- Replace fx_1 with the ID of an existing Reverb effect.
effects.enable("fx_1", true)
effects.set("fx_1", "feedback", 0.7)
effects.set("fx_1", "wet_mix", 0.4)
```

`effects.toggle("fx_1")` flips the current enabled state.
`effects.enable("fx_1", false)` bypasses an ordinary effect; **Cut is different**:
enabled means muted and disabled means passing audio.
`effects.set` changes a numeric parameter, not the effect's type or graph
connections. Use these parameter keys with the ranges above:

| Effect | Lua parameter keys (in table order above) |
| --- | --- |
| Gain | `level` |
| Distortion | `input_gain`, `drive`, `clip`, `output_gain` |
| Pitch | `input_gain`, `semitones`, `mix`, `output_gain`, `noise_floor`, `wet_lpf_hz` |
| Reverb | `input_gain`, `feedback`, `damping`, `wet_mix` |
| Noise Gate | `threshold_db`, `attack_ms`, `release_ms`, `range_db` |
| Cut | `channels` |

Values use actual units, not normalized CC values: a Pitch `semitones` value of
`7` means seven semitones, not 70%. Unknown IDs, wrong parameter keys, and
out-of-range values produce runtime errors rather than silently clamping.
Changing Cut's channel count affects routing shape; keep its connections
consistent rather than treating it as a continuously adjustable sound control.

Unlike the WebUI sliders, **Lua actions do not save effect settings**. They are
queued runtime changes; restarting reloads the saved configuration. Actions
already played cannot be rolled back if a later action fails.
See the [Lua API](/lua/api#effects-and-volume) and
[effect cookbook recipes](/lua/cookbook#toggle-an-effect) for more examples.

## Troubleshooting

- **No effects are listed:** create one from Routing's Add menu. The Effects
  page is a manager; it does not add an effect itself.
- **A parameter slider is missing:** the controls change with effect type. Cut
  has only a channel-count control; its mute control is on Effects.
- **The sound does not change:** verify the effect is connected into a
  source-to-destination path and is enabled. For Cut, check that it is not
  muted. A bypassed ordinary effect passes audio without processing.
- **A route disappears after changing Cut channels:** reconnect the channel
  sockets that still exist and save routing again.
- **MIDI mapping waits indefinitely:** confirm a MIDI controller is connected,
  then send a new CC or note after pressing **Map**. Existing or stale events
  are not captured as the new mapping.
- **The LED does not reflect state:** check that the selected MIDI controller
  is connected and that the configured velocity is nonzero. Use **Default**
  to restore the global values.
- **Audio is distorted or too loud:** lower effect input/output gain, drive,
  or the source node's level; keep the channel path simple while testing.
  Routing's browser graph has no live meters; see
  [Routing](./routing.md) and [Troubleshooting](./troubleshooting.md).

Related guides: [Getting started](./getting-started.md),
[Routing](./routing.md), [MIDI](./midi.md), [Macros](./macros.md),
[Configuration](./config.md), and [Development](./development.md). The
[Reference](./reference.md) groups effect API and configuration file details.
