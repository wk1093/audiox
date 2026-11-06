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
There is no separate Save button for parameter changes.

## Names and precise controls

Effects start with readable names such as **Reverb 3** or **Noise Gate 4**.
On **Effects**, enter a name such as **Mic Reverb** and choose **Save name**.
The name is saved across restarts and appears in Routing,
Volume, and effect notifications. Clear the name and save to restore the
automatic name. Names allow 63 UTF-8 bytes and cannot contain control characters.
Names need not be unique; use distinct names when building larger patches.

Each parameter has a slider, a precise numeric input, a unit where applicable,
and an **i** info control beside its label. Hover the info control for a tooltip,
or click/tap it to open the explanation. Keyboard users can focus it and press
Enter or Space; Escape closes the explanation and returns focus to the control.
**Reset** restores
that parameter's factory default and saves it. Gain values use multipliers:
`1x` is unchanged. Mix values remain fractions from 0 to 1, not percentages.
The pitch noise-floor control retains four decimal places for fine adjustment.

Expand **MIDI controls and pad lighting** only when assigning hardware controls.
MIDI and advanced sections retain their expanded state while Effects refreshes.
**Advanced and delete** holds the stable `fx_N` scripting/routing ID and Delete.
Renaming never changes IDs, script references, audio parameters, or connections.
The Volume card's **Advanced** disclosure also shows its scripting/routing ID.

## Add, connect, and edit an effect

1. On **Routing**, right-click empty graph space, open **Add Node** → **effects**, and choose the effect type.
2. Connect the incoming route to the effect's input channel and the outgoing
   route from its matching output channel. Effects preserve channel positions;
   connect channel 0 to channel 0, channel 1 to channel 1, and so on.
3. Adjust the parameter sliders displayed on the effect node. Changes are
   applied after a short debounce and saved automatically.
4. On **Effects**, use **Bypass**/**Enable** for ordinary effects. A Mute (Cut) node
   uses **Mute**/**Unmute** instead. Tune parameters here as well; expand the
   MIDI or advanced sections for mapping, lighting, and deletion.

Deleting an effect from the Effects page removes its saved effect state and its
node from Routing. Check the graph and save the route changes you intend to
keep.

## Custom native plugins

The **Plugins** card installs `.axm` packages, lists their metadata, reloads
faulted workers, and removes unused packages. Installed plugins appear in the
Routing Add menu and use normal parameter sliders, MIDI mappings, and Lua calls.
Optional **Open custom UI** controls run in an isolated iframe.

Native DSP runs in a sandboxed worker, adding one block (about 2.67 ms at
48 kHz/128 frames) per enabled plugin instance. A late block uses delayed dry
audio; a repeatedly late or crashed worker stays bypassed until reloaded.
Remove all live/saved effect instances before removing a package.
See [Plugin development and package format](../plugins) for examples, limits,
kernel requirements, and storage/backup caveats.

## Built-in effect controls and defaults

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
|  | Quality | High / Balanced / Economy | High |
| **Reverb** | Input Gain | 0–4 | 1 |
|  | Feedback | 0.05–0.95 | 0.64 |
|  | Damping | 0–1 | 0.48 |
|  | Wet Mix | 0–1 | 0.26 |
| **Noise Gate** | Threshold dB | −80–0 dB | −62 dB |
|  | Attack ms | 0.2–100 ms | 2 ms |
|  | Release ms | 5–600 ms | 160 ms |
|  | Range dB | 0–80 dB | 30 dB |
| **Cut** | Channels | 1–16, whole numbers | 2 |
| **Compressor** | Threshold | −60–0 dBFS | −18 |
| | Ratio | 1–20:1 | 4 |
| | Attack | 0.1–100 ms | 10 |
| | Release | 5–1,000 ms | 120 |
| | Knee | 0–24 dB | 6 |
| | Makeup | −12–24 dB | 0 |
| | Mix | 0–1 | 1 |
| **Limiter** | Ceiling | −24–0 dBFS | −1 |
| | Release | 5–1,000 ms | 80 |
| | Lookahead | 0–10 ms | 3 |
| | Input Gain | 0–4 | 1 |
| **Parametric EQ** | Low Hz / Gain | 40–1,000 Hz / −18–18 dB | 150 Hz / 0 dB |
| | Mid Hz / Gain | 200–8,000 Hz / −18–18 dB | 1,000 Hz / 0 dB |
| | High Hz / Gain | 1,000–18,000 Hz / −18–18 dB | 6,000 Hz / 0 dB |
| | Shared Q | 0.2–10 | 1 |
| | Output Gain | 0–4 | 1 |
| **Multiband Compressor** | Low Crossover | 80–1,000 Hz | 200 |
| | High Crossover | 1,000–10,000 Hz | 2,500 |
| | Low / Mid / High Threshold | −60–0 dBFS each | −18 |
| | Shared Ratio | 1–20:1 | 4 |
| | Release | 5–1,000 ms | 120 |
| | Makeup | −12–24 dB | 0 |
| **Noise Reduction** | Reduction | 0–30 dB | 12 |
| | Starting Noise Floor | −90–−20 dBFS | −55 |
| | Adaptation | 0–1 | 0.1 |
| | Smoothing | 0–1 | 0.7 |
| | Mix | 0–1 | 1 |
| | Output Gain | 0–4 | 1 |

Cut's **Channels** setting changes how many input and output sockets the node
has. If the count changes, reconnect routes that refer to channels that no
longer exist. A newly created Cut is in its passing (not muted) state.

### Algorithms, headroom and latency

- **Compressor** uses a sample-peak detector with soft knee, gain-reduction
  attack/release, makeup and parallel dry/wet mixing. **Limiter** caps sample
  peaks, not intersample/true peaks. It adds its selected lookahead delay;
  changing lookahead clears history, so a brief gap is possible.
- **Parametric EQ** has three peaking bands and a shared Q, with a short
  coefficient-change crossfade. **Multiband Compressor** uses three
  Linkwitz-Riley bands, fixed 10 ms attack and 6 dB knee. Ratio 1 and makeup 0
  have unity magnitude but allpass phase, not sample-identical bypass.
  Frequencies are bounded below Nyquist; the high crossover stays above the low.
- Dynamics detectors are independent per channel, not stereo-linked. Compression,
  makeup and EQ boosts can overload the next node or final output; reduce gain
  or add a final limiter. These three effects add no explicit block delay.
- **Noise Reduction** suppresses estimated stationary background noise, such as
  hiss or fans. It is not acoustic echo cancellation or an ML voice separator.
  Set the starting floor below quiet speech; excessive reduction can introduce
  watery/musical artifacts. Adaptation 0 freezes the estimate. Its phase-preserving
  spectral processor adds **256 frames** (5.33 ms at 48 kHz), including dry mix.
- **Pitch** uses a 1024-sample phase vocoder. **High** uses 8x overlap (128-frame
  hop), **Balanced** uses 4x (256-frame hop), and **Economy** uses 2x (512-frame
  hop). Balanced halves the FFT frame rate; Economy quarters it, with more
  modulation, weaker separation of nearby tones, and possible level loss.
  Economy follows local spectral-peak frequencies to limit phase-unwrapping
  artifacts at its larger hop. These are CPU/quality tradeoffs, not guarantees
  against X-runs: an FFT frame still runs in a single processing block.
  High remains the default for existing configurations and scenes.
  Changing Quality clears the effect's history and can briefly interrupt audio.
  Shifted audio
  and its internal dry mix have **1024 frames** of delay (21.33 ms at 48 kHz).
  Zero semitones or Mix 0 are immediate, transparent apart from input/output
  gain and peak clamping. Switching between these and shifted processing clears
  history and changes latency. Pitch remains non-formant-preserving and can
  sound phasey, soften transients, or lose level on upward shifts. Overlap-add
  normalization is corrected so downshifts no longer receive the old unintended
  level boost.

Delays affect only routes through the corresponding enabled effect, never
unrelated routes. Bypass is immediate and clears effect history. There is no
automatic latency compensation between parallel graph routes; mixing an external
dry route with a delayed effect can comb-filter. Bypass is a direct graph
connection, without a per-effect copy or clamp; final device output still clamps
to its PCM range. An enabled Cut is a disconnection, not a DSP zero-fill pass.
Built-in silent branches skip DSP once their tails and control release settle;
see [Runtime optimizations](./routing.md#runtime-optimizations).
Re-enabling or changing type
does not replay old tails.

## Audio idle sleep

After **5 seconds** without signal above **-60 dBFS** (peak amplitude `0.001`)
on a path to an output, the audio engine enters idle sleep and skips graph
mixing and effects. This short timeout is
for testing. Capture, playback of silence, Bluetooth buffer servicing,
soundboard timing, and input meters continue; this is not board suspend.

A live microphone behind an enabled Cut, a zero-level Gain effect, or a
zero-volume source does not prevent sleep unless another route carries its
signal to an output. A noise gate is not treated as a hard mute. Live sources
and outputs at or below the noise floor count as quiet, so analog background
noise need not prevent sleep. This intentionally suppresses very quiet real
audio while asleep; input must exceed the floor to wake processing.

Reverb, pitch, EQ, multiband, limiter and noise-reduction audio memory must settle before an audible path can sleep;
residuals at or below the same `0.001` noise floor are treated as silent.
Muted/disconnected tails are cleared on sleep entry rather than frozen and
replayed later. New signal on
a live route, soundboard triggers, routing changes, or effect/volume changes
resume processing without waiting for another timeout. Sleep and wake
transitions do not produce log messages.

This mode reduces DSP work, not audio I/O wakeups. Power savings and glitch-free
operation still require testing with the connected hardware.

## MIDI controls and effect LEDs

Connect the MIDI controller you want to use, then open **Effects**:
Expand the effect's **MIDI controls and pad lighting** section.

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
[Scripts](./macros.md) for automation.

## Control effects from Lua

**Yes: Lua scripts can toggle effects, explicitly enable/bypass them, and change
their parameters.** A web-triggered effect script does not need a MIDI controller
or the USB keyboard function.

Create and connect the effect in Routing first. Find its exact `fx_N` ID in
`GET /api/effects`, then use it in a callback on **Scripts**:

```lua
-- Replace fx_1 with the ID of an existing Reverb effect.
hook("run", function(event)
    effects.enable("fx_1", true)
    effects.set("fx_1", "feedback", 0.7)
    effects.set("fx_1", "wet_mix", 0.4)
end)
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
| Pitch | `input_gain`, `semitones`, `mix`, `output_gain`, `noise_floor`, `wet_lpf_hz`, `quality` (0 High, 1 Balanced, 2 Economy) |
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
[Routing](./routing.md), [MIDI](./midi.md), [Scripts](./macros.md),
[Configuration](./config.md), and [Development](./development.md). The
[Reference](./reference.md) groups effect API and configuration file details.
