# MIDI controllers and USB passthrough

AudioX uses a connected raw-MIDI controller for soundboard pads, action buttons,
effect bypass, volume/effect CC control, and macro triggers. Supported USB
controllers need to appear as compatible ALSA raw-MIDI devices. A controller
requiring proprietary drivers or a proprietary lighting protocol may not work
as expected.

The current device discovery selects a controller automatically, preferring
USB candidates, and excludes AudioX's own gadget MIDI endpoint. There is no
WebUI controller chooser or multi-controller MIDI patchbay.

## What goes on each page?

| Task | Page |
| --- | --- |
| Bind a sound to a pad | [Soundboard](./soundboard), Edit mode |
| Map Stop All or Sampler Toggle | MIDI, Actions |
| Set sampler keyboard channel/root | MIDI, Sampler |
| Set global controller colors and Dim lights | MIDI, Lighting |
| Bind a knob to a volume | [Volume](./volume) |
| Map a parameter, bypass note, or effect LED | [Effects](./effects) |
| Bind a named automation to a channel/note | [Macros](./macros) |
| Expose USB MIDI or keyboard to the computer | [Config](./config) |

Connecting a MIDI controller does not automatically forward it to a host computer.
Local mappings work independently of USB MIDI passthrough.

## Notes, channels, and learning

Note numbers and CC numbers use **0-127**. The channel fields in AudioX use
**0-15**; a controller that calls its first channel "1" corresponds to AudioX
channel **0**.

Soundboard, global actions, and effect bypass mappings are note-number-based.
Volume and effect parameter mappings are CC-number-based. These mappings are
not channel-isolated in the same way that macros and the sampler keyboard are.

When assigning a pad or knob, first click the relevant **Assign** or **Learn**
control, then move the hardware control. The WebUI snapshots the latest event
before waiting so an old press is not intentionally reused. Check the resulting
number, and cancel learning before using unrelated controls.

For macros, learning fills **both** channel and note; you still need **Save Macro**
to store the edited definition. Ordinary sound/action assignment saves when the
new note is captured.

## Global actions

Under **MIDI > Actions**:

1. Choose **Assign MIDI** beside **Stop All** or **Sampler Toggle**.
2. Press the desired pad.
3. Verify the displayed note.
4. Choose **Clear** to remove that assignment later.

**Stop All** stops all soundboard voices. It does not mute all routed audio.
Put it on an easy-to-reach pad when performing.

**Sampler Toggle** toggles a runtime selection mode. While selection mode is on,
pressing a sound-mapped pad selects that sound for pitched keyboard playback
instead of playing it normally. It is not a MIDI recording function.

## Play a sound at different pitches

1. Upload a short clip and map it to a soundboard pad.
2. Assign a separate pad to **Sampler Toggle**.
3. Under **Sampler**, enable **Keyboard Enabled**, choose a keyboard channel,
   set the root note, and choose **Save**.
4. Press the Sampler Toggle pad to enter selection mode.
5. Press the sound's mapped pad to select it.
6. Play notes on the configured keyboard channel.
7. Toggle selection mode off when you want ordinary sound-pad playback again.

The root note plays at the original rate. Notes above and below it change the
playback ratio by semitone intervals. This changes pitch and duration together;
it is sample-rate-style pitch playback, not time-stretched sampling.

Pitched notes start one-shot playback. Note-off does not implement a sustained
polyphonic sampler envelope. The selected sound and selection-mode flag are
runtime state; do not rely on them surviving restart.

::: tip Reserve separate channels and notes
Once a sample is selected, note-on messages on the enabled sampler keyboard
channel take priority over ordinary sound/action/effect-toggle handling.
Use a different channel for pads and the sampler keyboard, and avoid putting
Stop All or Sampler Toggle on notes intercepted by the keyboard. Macros are
matched separately and can still be queued by their own bindings.
:::

Disabling **Keyboard Enabled** disables the pitched keyboard path, not MIDI
input generally. **Reload** reads the saved sampler configuration.

## Controller lighting

AudioX sends note-on messages back to the selected controller. The global
**MIDI Channel** chooses the LED output channel. Velocities are controller-specific
color/state values:

- **0** means off.
- **Mapped** is an assigned sound pad while idle.
- **Playing** is a running clip, or the active held pad.
- **Stop All** is the global stop-action pad.

Choose **Save Lighting** to store the values, and **Refresh LEDs** to resend
the current desired state. Some controllers need a particular mode or channel
before incoming notes affect LEDs; check the controller's own documentation.
AudioX does not translate velocity values into named RGB colors.

Advanced clients can set per-sound lighting overrides through the
[HTTP interface](./reference#soundboard-and-midi). There is no dedicated
per-sound color editor on the current soundboard cards.

Effect LEDs use their own enabled/bypassed configuration. MIDI-bound macros have
idle and pressed colors, and Lua can temporarily override its bound pad.
The resulting LED precedence is sound state, then global action, then effect
bypass, then a valid MIDI-bound macro on the same note. Avoid overlapping notes
if you need an unambiguous indication.

**Dim lights** temporarily blanks all controller LEDs without stopping MIDI
control or audio. **Restore lights** restores the computed colors. Dimming is
not saved or included in backups and resets at reboot; reconnecting the
controller or refreshing LEDs does not silently undim it.

## Forward MIDI to a USB host

In **Config**, set **USB MIDI Passthrough**:

| Mode | Result |
| --- | --- |
| Disabled | No USB MIDI gadget function |
| Unmapped messages only | Forward controller messages that AudioX does not claim as local mappings |
| All messages, including mapped | Forward local-control messages too; one press can affect both AudioX and the host |

Choose **Save Config**, then **Reload Config**. All USB gadget functions briefly
disconnect, including networking, so expect a short interruption.

Forwarding is **one way: selected controller to USB host**. Host MIDI is not
routed back into AudioX, to controller LEDs, or to local macro triggers.
Lua's `midi.*` functions send to the **controller**, not this host endpoint.

Start with **Unmapped** to avoid a pad triggering a sound both locally and in
host software. The reserved sampler channel is treated as mapped while enabled.
Messages are queued nonblockingly; overload/disconnection cleanup is best effort,
not a guarantee that a disconnected host receives every final note-off.

## Troubleshooting

- **Waiting for MIDI device:** check the data cable, power, USB host connection,
  controller compatibility, and logs. A charge-only cable cannot carry MIDI.
- **The wrong pad triggers:** verify zero-based channels for macros/sampler,
  and remember ordinary note/CC mappings are not channel-specific.
- **A pad does two things:** check macros as well as sound/action/effect mappings,
  and check whether passthrough is set to All.
- **No LED response:** verify the controller's note-driven LED protocol and output
  channel, choose Refresh LEDs, and check Dim lights.
- **The host sees no MIDI device:** enable passthrough and apply Config; merely
  connecting a controller to the Pi is not sufficient.

For automation, continue to [Macros](./macros) and the [Lua reference](/lua/api).
