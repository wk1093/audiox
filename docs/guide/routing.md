---
title: Audio routing
description: Connect AudioX audio devices, effects, and channels with the Routing graph.
---

# Audio routing

The **Routing** page is a patch bay for the audio devices and processing nodes
currently available to AudioX. A route connects one node's output channel to
another node's input channel. Saved routes take effect immediately and are
restored when AudioX restarts.

## Build a route

1. Open **Routing**. AudioX loads the current audio devices, effects, and saved
   connections. If the list is empty, check that the audio subsystem has started
   and reload the page.
2. Find an input node, such as **USB Gadget In**, a **BT … In** node for a
   paired Bluetooth device, or an `… In` ALSA device. Find a destination such
   as **USB Gadget Out**, a **BT … Out** node, or an `… Out` ALSA device.
3. Drag from an output socket on the source node to an input socket on the
   destination. The socket labels are one-based (`out 1`, `in 1`); the channel
   numbers shown in route details and saved files are zero-based (`[0]` is the
   first channel).
4. To change a connection, click its wire or its row in **Routes**, then remove
   it. To remove a wire directly, click near the wire. Device nodes cannot be
   deleted. Remove effects from **Effects**; remove a routing helper from its
   node menu.
5. Choose **Save Routing**. AudioX writes the connections and reloads the graph;
   the new routing is live without a reboot.

The **Routing Overview** counts nodes and routes and identifies a selected node
or route. Selecting a route shows its source and destination channel. The graph
canvas itself does not show live per-node level meters. AudioX's device display
has a separate meters view; its display mode can be switched from the
Soundboard page.

## Channel paths and saved syntax

A route is directed. Its channels are written as
`edge=source,destination,source-channel,destination-channel`. For example:

```text
# Bluetooth input, through effect fx_1, to the USB audio host
edge=bt_A4C6F0A57AA7_in,fx_1,0,0
edge=fx_1,usb_gadget_out,0,0
```

In the UI, connect the matching output and input sockets; you do not need to
edit this file. The example uses channel 0 at each step. For stereo, make a
separate route for channel 1:

```text
edge=bt_A4C6F0A57AA7_in,fx_1,1,1
edge=fx_1,usb_gadget_out,1,1
```

### Common graph IDs

| Graph ID | Direction in Routing | What it represents |
| --- | --- | --- |
| `usb_gadget_in` | Source (outputs) | USB host/computer playback entering AudioX. |
| `usb_gadget_out` | Destination (inputs) | AudioX playback sent to the USB host/computer. |
| `soundboard_out` | Source (outputs) | Soundboard audio. |
| `bt_<MAC>_in` | Source (outputs) | Audio received from one paired Bluetooth device, such as a phone. |
| `bt_<MAC>_out` | Destination (inputs) | Audio sent to one paired Bluetooth headphone or speaker. |
| `routing_helper` | Two inputs and two outputs | Optional two-channel pass-through node. |
| `fx_1`, `fx_2`, … | Processing node | Effect IDs assigned when effects are created. |
| `alsa_<stable-card-id>_devN_in` | Source (outputs) | Capture channels from a detected ALSA device. |
| `alsa_<stable-card-id>_devN_out` | Destination (inputs) | Playback channels to a detected ALSA device. |

Each paired Bluetooth device gets its own nodes. `<MAC>` is the device
address as 12 uppercase hex digits without separators, so the phone
`A4:C6:F0:A5:7A:A7` appears as `bt_A4C6F0A57AA7_in`. Devices that only send
audio, such as phones, get only an `_in` node. Devices that only receive
audio get only an `_out` node. A device that AudioX has not yet identified
gets both. A Bluetooth node passes audio only while that device is
connected; otherwise its input node is silent and its output node discards
audio.

Routes to a Bluetooth device that is not paired (for example after
**Forget**) are not shown in the graph. **Save Routing** keeps them in the
file untouched, and they come back when the device is paired again. Older
configurations that used the single `bluetooth_in` or `bluetooth_out` node are
converted to one route per paired device with that role. If no such device is
paired yet, the old lines are kept until one is.

The ALSA ID pattern is illustrative; the card ID and device number depend on
the connected hardware. Use the exact ID shown in your graph. Effect IDs are
also dynamic: use the ID on the created effect node rather than assuming it
will be `fx_1`.

The Config channel labels use AudioX's playback/capture stream direction,
which is the reverse of how a USB host describes those same streams:

- **USB Playback Channels** is audio played by AudioX to the host. On the host
  this is a capture input; the route leaves AudioX through the
  `usb_gadget_out` destination.
- **USB Capture Channels** is audio captured by AudioX from the host. On the
  host this is playback output; the route enters AudioX at the
  `usb_gadget_in` source.

For example, to listen to host playback through an ALSA device, route from
`usb_gadget_in` to that device's `…_out` node. To record Bluetooth input on
the host, route from the device's `bt_<MAC>_in` node to `usb_gadget_out`. The USB node names
describe AudioX/gadget stream direction, not the host application's playback
or recording action.

Multiple routes can leave a source or feed an input. Inputs receiving several
routes are summed. The **Routing Helper** is a two-channel pass-through node;
it forwards each input channel to the matching output channel. It is useful
for organizing a patch, but it does not mix channels or add gain. Its
configuration is represented by a separate line:

```text
node=routing_helper
edge=bt_A4C6F0A57AA7_in,routing_helper,0,0
edge=routing_helper,fx_1,0,0
edge=fx_1,usb_gadget_out,0,0
```

Create a helper by adding **Routing Helper** from the graph's Add menu. Saving
the graph saves both its enabled state and its edges. Removing the helper
removes the node; review and save any remaining routes that you want to keep.

## Arrange and navigate the graph

- **Arrange** places source/device nodes, the helper, effects, and destinations
  in columns. Drag node headers to position them manually.
- **Fit** scales and centers all nodes in the current graph area. The `+`,
  `−`, and percentage buttons zoom; scrolling over the canvas also zooms.
- **Fullscreen** expands the graph area. **Details** shows or hides the node,
  route, and selection information.

Layout and zoom are presentation controls, not routes. **Save Routing** saves
connections and helper presence; it does not save node positions.

## Effects, limits, and rejected connections

Add Gain, Distortion, Pitch, Reverb, Noise Gate, or Cut from the graph Add menu.
The effect is created as an `fx_N` node; connect it like any other
channel-preserving processing node. Effect parameters and bypass are managed
on its node and on **Effects**. See [Effects](./effects.md).

The graph is rebuilt against the devices and channel counts currently detected
by AudioX. A saved edge is skipped when its endpoint is unavailable, its
channel is out of range, its text is malformed, or the graph has reached its
edge capacity. If no saved edge is valid, AudioX rejects the routing update;
if only some are invalid, the valid edges can still load while invalid ones are
skipped. A route that used to work can therefore become inactive after
unplugging a device or changing a Cut effect's channel count. Reconnect the
device or correct the affected channels, then save again.

Keep signal flow one-way from inputs through processing to outputs. AudioX
orders processing nodes for chained effects, but it does not provide a
feedback-loop design or a cycle-warning workflow. Avoid connecting a path back
into an earlier node.

## Troubleshooting

- **No devices or missing ALSA node:** check that the interface is connected
  and supported, then reopen **Routing** so the current topology is discovered.
  Device names and channel counts come from detected hardware.
- **A wire will not connect:** start at an output socket and finish at an input
  socket. Verify that the source has outputs, the destination has inputs, and
  the desired channel numbers exist on both.
- **A saved connection is missing or silent:** verify the device is present and
  the channel indices are valid. A disconnected device's route is unavailable
  until the device returns. A changed Cut channel count can also invalidate
  downstream edges.
- **The whole graph fails to reload:** remove malformed, unavailable, or
  out-of-range routes; reconnect nodes in a simple forward path and use
  **Save Routing** again.
- **No level appears in the graph:** the browser graph is not a meter display.
  Use the device display's meters view, or check the [System](./system.md) and
  [Troubleshooting](./troubleshooting.md) guides for audio health and logs.

Related guides: [Getting started](./getting-started.md),
[Soundboard](./soundboard.md), [MIDI](./midi.md), [Macros](./macros.md),
[Configuration](./config.md), [Hardware](./hardware.md), and
[Development](./development.md). The planned [Reference](./reference.md) will
group the relevant API endpoints and configuration files.
