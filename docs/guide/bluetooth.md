---
title: Bluetooth audio
description: Pair Bluetooth audio devices, stream in or out, and troubleshoot codec and connection limits.
---

# Bluetooth audio

AudioX supports Bluetooth Classic audio roles in both directions. Route
**Bluetooth In** to an output to receive audio from another device, or route a
source such as **Soundboard Out** to **Bluetooth Out** to send audio to a
headphone or speaker. **Bluetooth In** and **Bluetooth Out** are separate
Routing nodes; pairing a device does not create an audio route automatically.

## Find and pair an audio device

1. Put the Classic Bluetooth audio device in pairing mode.
2. Open **Bluetooth** and choose **Add device**. AudioX scans nearby devices
   and lists discovered devices under **Nearby devices**.
3. Choose **Pair** on the target device. Wait for the status to report the
   result. Successfully paired devices appear under **My devices**.
4. For an audio output (headphones or a speaker), choose **Connect** on the
   paired device. AudioX can connect to one Bluetooth output at a time.
5. For an audio input source, such as a phone sending audio to AudioX, pair it
   first, then initiate its audio connection from that device. AudioX presents
   a Bluetooth audio sink and provides **Bluetooth In** as a Routing source.
6. Connect the relevant Routing node to the desired destination and save the
   route. To stop sending to an output, choose **Disconnect**.

Bluetooth status and device lists refresh while the page is open. The page
separates **My devices** (paired or connected) from **Nearby devices**
(discovered but not paired). It offers **Pair**, **Connect**, **Disconnect**,
and **Forget** as appropriate. **Forget** removes the saved pairing; you must
pair again before reconnecting.

**Hide unnamed devices** is on by default. It hides nearby results without a
usable name (including names that are just the device address), but never hides
saved Classic devices. Turn it off to inspect address-only results; the list
updates immediately without another scan. This is a page-view filter, not a
saved device setting, and resets to on when you reload the WebUI.

An unnamed result does not prove that the device is out of pairing mode.
Names may be omitted from discovery replies or remote name lookups may time out.
Discovery also does not prove audio support or readiness to pair: a Classic
device can be discoverable without accepting a new pairing. Put the intended
audio device into pairing mode before choosing Pair.

The page's **Connected** state describes AudioX's outgoing connection to a
paired audio sink. It does not report whether a phone or other source is
currently streaming into **Bluetooth In**.

## What AudioX saves and supports

Pairing keys and device names are saved on the AudioX device and reloaded on
startup, so previously paired devices remain in **My devices**. A saved
pairing is not the same as an active audio connection: choose **Connect** for
an output when needed. Configuration backups include Bluetooth pairing data;
the archive contains pairing keys, so keep it private.

Bluetooth audio uses the SBC codec. AudioX does not offer a codec selector or
AAC/aptX/LDAC support. For outgoing audio, the remote sink must support
48 kHz stereo SBC; AudioX selects a low SBC bitpool for its output stream.
AudioX advertises 48 kHz joint-stereo SBC when receiving audio.

Discovery uses **Classic inquiry and bounded remote name lookups only**. AudioX
does not run BLE discovery, so BLE-only advertisements do not fill the list.
Dual-mode audio devices can still appear through their Classic discoverable
mode.

[Bluetooth LE Audio](https://www.bluetooth.com/learn-about-bluetooth/feature-enhancements/le-audio/)
can carry audio, but it is a separate technology using LC3 rather than AudioX's
Classic A2DP/SBC implementation. AudioX does **not** implement LE Audio or
Auracast. Generic BLE support or the ability to see an advertisement would not
make those devices usable for audio here.

Only one outgoing Bluetooth audio device can be connected at a time. Scanning
and pairing are unavailable while that output is connected; disconnect it
before adding another device.

## Route Bluetooth audio

To play a paired headphone or speaker, connect it on the Bluetooth page, then
route a source to `bluetooth_out`, for example:

```text
edge=soundboard_out,bluetooth_out,0,0
edge=soundboard_out,bluetooth_out,0,1
```

To receive a stereo stream from another device and send it to the USB host:

```text
edge=bluetooth_in,usb_gadget_out,0,0
edge=bluetooth_in,usb_gadget_out,1,1
```

The Soundboard currently uses only the first sample channel of a clip, even
when the clip is stereo. Its Routing node exposes that signal on channel 0, so
the first example fans channel 0 out to both sides of the Bluetooth output.
Bluetooth input is a stereo source, so the second example keeps its two
channels separate. These are zero-based channel numbers. The device names and
channel sockets shown in Routing are authoritative for the detected graph. Read
[Routing](./routing.md) for channel paths, helper nodes, and graph errors.

## Troubleshooting

- **Adapter not ready:** check that the AudioX Bluetooth adapter is available
  and enabled, then reopen Bluetooth. See [System](./system.md) for device
  status and logs.
- **Scan finds nothing:** put the device in discoverable/pairing mode, move it
  closer, and scan again. AudioX retains saved pairings even when those
  devices are not currently discoverable. Try turning off **Hide unnamed
  devices** if the target has no reported name.
- **LE-only device is missing:** this is intentional. Use a device with Classic
  A2DP/SBC support and enable its Classic pairing mode if it has multiple modes.
- **Cannot scan or pair:** disconnect the current outgoing Bluetooth audio
  device first. Wait for any scan, pairing, or connection operation to finish
  before starting another.
- **Pairing succeeds but there is no sound:** pairing only saves credentials.
  For output, choose **Connect** and route a source to `bluetooth_out`. For
  input, start the audio connection from the sending device and route
  `bluetooth_in` to a destination. Confirm both stereo channel routes.
- **Output connection fails:** confirm the device is paired, supports A2DP
  Sink with 48 kHz stereo SBC, and is not already connected elsewhere. If
  needed, disconnect, choose **Forget**, put it back in pairing mode, and pair
  again.
- **Sound breaks up:** confirm the route is correct, keep the wireless device
  close to AudioX, and try a fresh connection. Bluetooth streaming has bounded
  buffering; persistent output overruns stop the stream rather than continuing
  a severely glitching connection.
- **A phone is streaming in but the page says disconnected:** the page reports
  AudioX's outgoing link only. Check the receiving path through
  `bluetooth_in` and the selected destination instead.

Related guides: [Getting started](./getting-started.md),
[Routing](./routing.md), [Configuration](./config.md),
[Troubleshooting](./troubleshooting.md), [Hardware](./hardware.md),
[System](./system.md), and [Development](./development.md).
[Reference](./reference.md) groups Bluetooth API and persisted pairing-file details.
