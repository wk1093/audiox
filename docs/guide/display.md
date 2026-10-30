# Display and touch

AudioX can run without a display. Its framebuffer and touch UI are optional
companions to the WebUI, useful for monitoring and playing sounds directly.
They do not expose the full routing/effect/configuration editor.

Display compatibility depends on the framebuffer device, touch input, firmware
overlays, and build configuration. Do not assume that every HDMI or DSI panel
is plug-and-play: start with [hardware guidance](./hardware).

## Meter view

The default local view shows device/channel metering with system CPU and RAM
metrics. Scroll the device list when there are more devices than fit on screen.
Meters indicate signal activity; they are not a calibrated loudness measurement
or a guarantee that a downstream speaker is receiving audio.

The bottom controls are:

- **Sleep:** blank the UI until the next touch. Audio and control continue;
  this is not system suspend, shutdown, or guaranteed backlight power control.
- **Reboot:** restart the device.
- **Shutdown:** request power-off.

::: warning Local power actions
The local Reboot and Shutdown buttons act directly; do not expect the browser's
confirmation dialog on the touchscreen. Save work and stop recordings before
using them.
:::

## Soundboard view

On the WebUI's **Soundboard** page, choose **Show Soundboard on Display**.
Choose **Show Meters on Display** to return. The choice is saved on the Pi and
survives reboot; it is not just a browser view preference.

The local soundboard uses the same files, per-sound Play/Hold modes, and saved
order as the web soundboard.

- Tap a **Play** pad to trigger it on release, without a drag.
- Press a **Hold** pad to start it. Release or move out of its pad to stop the
  held voice.
- Use **STOP ALL** in the header to stop soundboard voices.
- Use the **<** and **>** header controls or swipe horizontally to change page.

The layout uses three rows: two columns on screens narrower than 480 pixels,
four columns on larger screens. Only pads on the current page can be touched.
Long names are shortened visually; use descriptive, distinguishable filenames.

File/order updates are checked about every two seconds and are deferred while a
touch is down, so a pad should not move underneath an ongoing press. If the
display is stale, release touch, wait, and refresh the web soundboard.

## Disconnecting and reconnecting a display

AudioX checks for missing or changed display/touch devices periodically
(normally on a **500 ms** device-maintenance interval), rather than constantly
reopening them. It releases framebuffer/touch resources when unavailable and
tries to reconnect. The audio path does not require the display to remain open.

This is resource management, not a promise that a particular panel supports
physical hotplug: some panels/overlays still need a reboot to appear correctly.

## Troubleshooting

- **WebUI works, display is blank:** check the Sleep state, panel power/cabling,
  framebuffer/overlay support, and boot logs.
- **Touch does not follow the screen:** verify compatible touch input and
  calibration/orientation at the driver/overlay level; there is no general
  calibration wizard on the WebUI.
- **Pads make no sound:** check the soundboard route, output volume, and cached
  clip. The touch display does not automatically connect an output.
- **A hold stops when another held pad is used:** there is one shared held
  voice across playback surfaces.

Use the [soundboard guide](./soundboard) for file management and
[System logs](./system) for diagnostics.
