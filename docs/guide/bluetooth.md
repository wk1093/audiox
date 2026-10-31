---
title: Bluetooth audio
description: Pair Bluetooth audio devices, stream in or out, and troubleshoot codec and connection limits.
---

# Bluetooth audio

AudioX supports Bluetooth Classic audio roles in both directions. Each paired
device gets its own Routing nodes: **BT <name> In** (`bt_<MAC>_in`) carries
audio received from that device, and **BT <name> Out** (`bt_<MAC>_out`) sends
audio to it. A phone, which only sends audio, gets only an In node; a
headphone or speaker gets only an Out node once AudioX has identified it. A
device whose role is not known yet shows both. Pairing a device does not
create an audio route automatically.

## Find and pair an audio device

1. Put the Classic Bluetooth audio device in pairing mode.
2. Open **Bluetooth** and choose **Add device**. AudioX scans nearby devices
   and lists discovered devices under **Nearby devices**.
3. Choose **Pair** on the target device. Wait for the status to report the
   result. Successfully paired devices appear under **My devices**.
4. Choose **Connect** on the paired device. AudioX connects in the direction
   the device supports: it sends audio to headphones and speakers, and
   receives audio from phones and other audio sources. For a device that does
   both, AudioX connects the output first, then the input on the same
   connection. AudioX can keep up to four Bluetooth devices connected at once
   (for example headphones and a phone), each with its own Routing nodes.
5. **Auto-connect** is on by default for saved devices. AudioX reconnects to
   enabled saved devices when the adapter is ready and retries each device
   with an increasing delay (up to a minute) if it is not yet available.
   Searching for an absent device ties up the radio for several seconds, so
   while audio plays to a Bluetooth output AudioX tries an absent device at
   most every 30 seconds, and while a phone or other source is connected as
   an input (playing or paused) AudioX does not search at all, because
   incoming audio would drop out. Headphones and speakers that are switched
   on during that time normally reconnect to AudioX by themselves; otherwise
   choose **Connect**. Turn Auto-connect off on a device
   card to prevent automatic reconnection. Choosing **Disconnect**, or
   disconnecting from the device itself (for example from the phone's
   Bluetooth menu), stops AudioX from reconnecting to that device until you
   choose **Connect**, re-enable Auto-connect, or the device connects to
   AudioX again. AudioX learns each device's direction when the device
   identifies as a phone, reports its audio endpoints, opens an audio stream
   into AudioX, or refuses AudioX's output connection twice in a row.
6. A paired phone or other audio source can also connect to AudioX by itself,
   for example by choosing AudioX in the phone's Bluetooth menu. AudioX is
   connectable by paired devices only; it is not discoverable, so new devices
   are always paired from the Bluetooth page.
7. Connect the relevant Routing node to the desired destination and save the
   route. To stop audio in either direction, choose **Disconnect**.

Bluetooth status and device lists refresh while the page is open. The page
separates **My devices** (paired or connected) from **Nearby devices**
(discovered but not paired). It offers **Pair**, **Connect**, **Disconnect**,
and **Forget** as appropriate. **Forget** removes the saved pairing; you must
pair again before reconnecting. While a connected Bluetooth output carries
only silence for a short period, AudioX pauses its A2DP media stream and
resumes automatically when routed audio returns. This releases the active
audio stream for device multipoint selection where supported; the Bluetooth
connection itself remains available.

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

Each device card reports both directions: **Connected as audio output** (with
**paused while idle** while AudioX has paused the stream) and **Connected as
audio input**, followed by **receiving audio** while the device is playing or
**waiting for audio** while it is connected but silent. Phones usually start
the stream only while something is playing.

## What AudioX saves and supports

Pairing keys, device names, and Auto-connect settings are saved on the AudioX
device and reloaded on startup, so previously paired devices remain in **My
devices**. Existing saved devices default to Auto-connect when upgraded.
Configuration backups include Bluetooth pairing data and Auto-connect
settings; the archive contains pairing keys, so keep it private.

Bluetooth audio uses the SBC codec. AudioX does not offer a codec selector or
AAC/aptX/LDAC support. For outgoing audio, the remote sink must support
48 kHz stereo SBC; AudioX selects a low SBC bitpool for its output stream.
When receiving audio, AudioX accepts SBC at 48 kHz or 44.1 kHz (most phones
send 44.1 kHz) and converts 44.1 kHz streams to the 48 kHz AudioX clock. It
prefers 48 kHz joint stereo when the source offers it. Received audio passes
through a buffer of about 120 ms that absorbs the uneven timing of Bluetooth
packets, so a Bluetooth input lags slightly behind wired inputs.

Discovery uses **Classic inquiry and bounded remote name lookups only**. AudioX
does not run BLE discovery, so BLE-only advertisements do not fill the list.
Dual-mode audio devices can still appear through their Classic discoverable
mode.

[Bluetooth LE Audio](https://www.bluetooth.com/learn-about-bluetooth/feature-enhancements/le-audio/)
can carry audio, but it is a separate technology using LC3 rather than AudioX's
Classic A2DP/SBC implementation. AudioX does **not** implement LE Audio or
Auracast. Generic BLE support or the ability to see an advertisement would not
make those devices usable for audio here.

Up to four Bluetooth devices can be connected at the same time, in any mix of
inputs and outputs. All of them share one Bluetooth radio, so several
simultaneous streams can cause dropouts, especially if a device is far away.
If audio stutters, disconnect devices you are not using. Scanning and pairing
are unavailable while any Bluetooth audio device is connected; disconnect all
devices before adding another one.

## Route Bluetooth audio

Node IDs contain the device address as 12 uppercase hex digits. To play to
the paired speaker `00:11:22:33:44:55`, connect it on the Bluetooth page, then
route a source to its Out node, for example:

```text
edge=soundboard_out,bt_001122334455_out,0,0
edge=soundboard_out,bt_001122334455_out,0,1
```

To receive a stereo stream from the phone `A4:C6:F0:A5:7A:A7` and send it to
the USB host:

```text
edge=bt_A4C6F0A57AA7_in,usb_gadget_out,0,0
edge=bt_A4C6F0A57AA7_in,usb_gadget_out,1,1
```

A device's nodes pass audio only while it is connected; at other times its In
node is silent. Routes for a device that is no longer paired disappear from
the graph but stay saved, and return if you pair it again. Saving routing does
not remove them. Older single `bluetooth_in`/`bluetooth_out` routes are
converted to per-device routes automatically. Volume settings, MIDI
mappings, and macros that used those older IDs must be pointed at the new
per-device IDs.

The Soundboard currently uses only the first sample channel of a clip, even
when the clip is stereo. Its Routing node exposes that signal on channel 0, so
the first example fans channel 0 out to both sides of the Bluetooth output.
Bluetooth input is a stereo source, so the second example keeps its two
channels separate. These are zero-based channel numbers. The device names and
channel sockets shown in Routing are authoritative for the detected graph. Read
[Routing](./routing.md) for channel paths, helper nodes, and graph errors.

## Troubleshooting

Bluetooth logs retain connection events and errors, but do not print periodic
healthy-stream statistics or scheduler/tuning diagnostics. Input statistics
are reported as warnings only when packets or audio frames are lost, decoding
fails, or the input buffer underruns or overflows. Packet gaps alone do not
produce a warning.

- **Adapter not ready:** check that the AudioX Bluetooth adapter is available
  and enabled, then reopen Bluetooth. See [System](./system.md) for device
  status and logs.
- **Scan finds nothing:** put the device in discoverable/pairing mode, move it
  closer, and scan again. AudioX retains saved pairings even when those
  devices are not currently discoverable. Try turning off **Hide unnamed
  devices** if the target has no reported name.
- **LE-only device is missing:** this is intentional. Use a device with Classic
  A2DP/SBC support and enable its Classic pairing mode if it has multiple modes.
- **Cannot scan or pair:** disconnect the connected Bluetooth audio devices
  first. Wait for any scan, pairing, or connection operation to finish
  before starting another.
- **Pairing succeeds but there is no sound:** pairing only saves credentials.
  For output, choose **Connect** and route a source to that device's
  `bt_<MAC>_out` node. For input, choose **Connect** (or connect from the
  sending device), start playback on it, and route its `bt_<MAC>_in` node to
  a destination. Confirm both stereo channel routes.
- **Output connection fails:** confirm the device is paired, supports A2DP
  Sink with 48 kHz stereo SBC, and is not already connected elsewhere. If
  needed, disconnect, choose **Forget**, put it back in pairing mode, and pair
  again.
- **Device reconnects unexpectedly:** turn off **Auto-connect** for that saved
  device. A manual disconnect stops retries for that device until you connect
  it again.
- **Phone shows "Connection refused" or "device refused the connection":**
  phones paired with older AudioX versions saw AudioX as a device without a
  speaker role and refuse its audio connection. On the phone, forget AudioX;
  on AudioX, choose **Forget** on the phone's card. Then put the phone in
  pairing mode and pair it again from **Add device**. A phone paired with an
  older version may first be tried as an output; AudioX recognizes it as an
  input after it reports its audio endpoints or refuses twice.
- **Input connects but stays "waiting for audio":** start playback on the
  sending device and check that it selected AudioX as its audio output.
- **Sound breaks up:** confirm the route is correct, keep the wireless device
  close to AudioX, and try a fresh connection. Bluetooth streaming has bounded
  buffering; persistent output overruns stop the stream rather than continuing
  a severely glitching connection.

Related guides: [Getting started](./getting-started.md),
[Routing](./routing.md), [Configuration](./config.md),
[Troubleshooting](./troubleshooting.md), [Hardware](./hardware.md),
[System](./system.md), and [Development](./development.md).
[Reference](./reference.md) groups Bluetooth API and persisted pairing-file details.
