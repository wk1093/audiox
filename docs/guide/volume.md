---
title: Volume
description: Set per-node AudioX gain and map MIDI CC controls to volume.
---

# Volume

The **Volume** page adjusts the gain associated with each currently available
Routing node in a mixer-style row of channel strips. It also lets you assign
one MIDI CC to one node. Open **Routing**
first if the Volume page says there are no active devices; Volume uses the
current routing graph's list of nodes.

## Set a level

1. Open **Volume** and find the channel strip by its readable name. Scroll
   horizontally to reach additional strips; on a touchscreen, swipe the mixer.
   Effect cards use the saved name from **Effects** (for example, **Mic Reverb**),
   or a default such as **Reverb 3**. Expand **Advanced** only when you need
   the stable scripting/routing ID.
2. Move the vertical fader up to raise gain or down to lower it. The displayed
   value is from **0%** to **200%**. The meter bars beside the fader rise from
   the bottom. Keyboard users can focus a fader and use the arrow keys.
3. The change is applied immediately and stored; there is no separate Save
   button. **Unity** resets a strip to **100%** (0 dB gain). Choose **Refresh**
   to reload the displayed values.

Effect channels are hidden by default. Enable **Show effect channels** to
adjust their outgoing gain or MIDI mapping. Hiding them does **not** disable
their saved gain, MIDI mappings, or script automation; the page reports how
many hidden effect channels have non-unity gain. The visibility choice is kept
while navigating within the current page session, but resets on a browser
reload. The audio routing engine already skips per-sample gain multiplication
at exactly unity, including for effect nodes.

Slider changes are saved by atomically replacing `volumes.txt` before applying
the new runtime gain. A failed save is reported as an error and does not apply
the requested gain. The API rejects non-finite numbers and malformed numeric
values rather than persisting them.

The slider's scale is AudioX gain: 100% is unity gain, 0% is silence, and the
slider's maximum (200%) is 2× gain. Boosting above unity can cause clipping.
Volume is a
per-routing-node control, not an independent hardware mixer. Gain is applied
to a node's outgoing routes; changing a terminal destination that has no
outgoing route does not attenuate the signal arriving at that destination.
For practical output-level control, adjust the source or an intermediate node
in the path.

If MIDI or an API client sets gain above unity, both the slider and percentage
label show the actual gain (for example, 150%). Viewing or refreshing the page
does not change that gain.

## Live meters

Each strip shows tightly packed, independently animated bars for each channel,
without per-channel text. A single compact reading below the bars averages the
channels' linear peak levels, then converts that average to dBFS. This includes
each paired Bluetooth device's input and output nodes. The average is not an
RMS loudness measurement; one loud channel can be diluted by quieter channels.
The individual bars retain each channel's own level. For nodes with many
channels, scroll the bar bank horizontally to see the remaining channels.
Meters fetch up to four snapshots per second.
Between snapshots, the bars animate with a fast rise and slower fall to make
changing levels easier to follow. Numeric dBFS readings show the latest
snapshot, not the animated value. Reduced-motion preferences disable this
animation. Switching pages or hiding the browser tab stops polling and
animation and clears the bars until fresh levels arrive. These are slower
snapshots than the local framebuffer meters and may miss short transients;
smoothing does not add measurements or hold missed peaks.

The bars use a -60 to 0 dBFS scale. Source and processing nodes show their
output signal before outgoing-route gain; destination nodes show incoming
signal. They monitor the routing graph, not acoustic loudness or confirmation
that a disconnected speaker is receiving audio. Node/channel changes refresh
the cards automatically. A failed request clears the meters and displays
**Meters unavailable** rather than leaving stale levels on screen. No routine
polling/status message is shown while meters are working.

The snapshot API is `GET /api/audio/meters`, returning
`{"meters":[{"id":"bt_A4C6F0A57AA7_in","levels":[0.5,0.25]}]}`.
Levels are linear per-channel peaks clamped to 0–1, not dB values.

## Assign a MIDI knob or fader

1. Connect the MIDI controller and open the node's **Assign CC** control.
2. Move the desired controller after the page says it is waiting. AudioX
   captures the next new CC number and shows it on that node.
3. To change the assignment, capture another CC for the node. **Clear CC**
   removes the existing mapping.

A CC value from 0 to 127 sets gain across the backend's full 0–2 range. Thus
the MIDI mapping can reach 200% gain, matching the web slider's maximum.
Assignment matches CC number; there is no per-MIDI-channel selector.
Each CC and each volume target can have only one volume mapping, so assigning
a CC already in use or assigning a second CC to the same target replaces the
conflicting volume assignment.

The mapping is saved with the MIDI configuration. Moving the mapped controller
changes the node's runtime gain; use **Refresh** to read back the current value
in the browser. The slider's explicitly set gain is saved in the volume
configuration and restored when AudioX starts. MIDI assignment does not
require USB MIDI passthrough; it uses the controller AudioX is already
receiving.

For automation, use the graph ID (for example, `usb_gadget_in`,
`soundboard_out`, `bt_A4C6F0A57AA7_in`, or an effect ID such as `fx_1`), not the
display name. The [Scripts](./macros.md) `volume.set` helper takes a percentage
from 0 to 100 and has a unity-gain maximum. A hardware MIDI CC volume mapping
has the wider 0–200% range described above. The planned
[Reference](./reference.md) will group API and persisted-file details.

For example, to set the gain on audio entering AudioX from the phone
`A4:C6:F0:A5:7A:A7` to 50% from a script, target its source-node ID:

```lua
volume.set("bt_A4C6F0A57AA7_in", 50)
```

Each paired Bluetooth device has its own volume entry. Volume settings,
MIDI mappings, and scripts that targeted the older shared `bluetooth_in` or
`bluetooth_out` IDs need to be pointed at the per-device `bt_<MAC>_in` or
`bt_<MAC>_out` ID.

## What is in the list?

Volume rows are generated from the active Routing graph. They can include
source, processing, helper, and destination nodes, and can change when devices
appear or disappear or when an effect's routing shape changes. For an ALSA
device, expand **Advanced** to find its ID and distinguish it from another
device with a similar name.

Because gain scales outgoing routes, source and intermediate nodes are the
useful points for adjusting a path. To change only the level of one signal
while leaving other signals unchanged, apply gain to that signal's source or
its own processing branch before it joins a shared route.

## Troubleshooting

- **No active devices:** open **Routing** and wait for its current device list,
  then return to Volume. Check the [Routing](./routing.md) guide if expected
  hardware is missing.
- **A slider changes but the sound does not:** make sure the node is on an
  active route and has an outgoing edge. A sink's own slider cannot change
  audio already arriving at it.
- **A MIDI assignment does not capture:** ensure the controller is connected,
  press **Assign CC**, then send a new CC event. If the mapping succeeds but
  the value looks stale, choose **Refresh**.
- **The MIDI range seems higher than the slider:** that is expected. A CC
  spans 0–200% gain, matching the browser slider.
- **A level returns after restart:** the web slider's level is persistent.
  MIDI controller movements change runtime gain but do not save each new CC
  value as the slider's stored level.

Related guides: [Routing](./routing.md), [MIDI](./midi.md),
[Scripts](./macros.md), [Configuration](./config.md),
[Soundboard](./soundboard.md), and [Troubleshooting](./troubleshooting.md).
