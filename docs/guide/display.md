# Display and touch

The framebuffer and touch UI provide local metering and soundboard controls.
They are optional and do not replace the WebUI's routing, effects, and
configuration editors.

The Pi 5 framebuffer UI remains **untested on hardware** because Pi 5 testing
was performed without a connected screen. Display rendering, touch interaction,
and display hotplug still need validation on that board; audio and WebUI
operation have been tested.

The UI draws through the kernel's KMS (DRM) display driver on both Pi 4 and
Pi 5, so DSI panels, DPI panels and HDMI monitors all work the same way; a
plain framebuffer (`/dev/fb*`) is only used for displays without a KMS driver,
such as some SPI panels. Drivers for the connected display and touch hardware
are loaded automatically. Panels that need a firmware overlay still need it
configured; see [hardware guidance](./hardware).

## Meter view

The default local view shows device/channel metering with system CPU and RAM
metrics. Scroll the device list when there are more devices than fit on screen.
Meters show signal activity, not calibrated loudness or proof that a downstream
speaker is producing sound.

Audio bars use the same logarithmic -60 to 0 dBFS scale as the web Volume
meters, with local attack/release smoothing. Signals at or below -60 dBFS
appear empty and full scale fills the bar. For example, -20 dBFS fills about
two-thirds of the bar, making quiet signals easier to see. This changes only
the display, not audio gain. CPU and RAM bars still show linear percentages.

Each paired Bluetooth audio device has its own row, including input-only
phones, output-only headphones, and devices supporting both directions.
Stereo input and output meters are separate; the row indicates whether the
device is paired or connected. Bluetooth rows follow wired devices.

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

HDMI connects and disconnects are reported by the kernel immediately, and the
UI moves to whichever display is connected (a built-in DSI/DPI panel is
preferred over HDMI when both are present). Large monitors run at their
preferred mode up to 1920×1200; above that the largest smaller mode is used.
USB touchscreens can be plugged in at any time. Touch coordinates are scaled to
the display, so a touch digitizer with its own resolution still lines up.

This is resource management, not a promise that a particular panel supports
physical hotplug. DSI and DPI panels are not hot-pluggable: connect them, and
keep them powered, before the Pi boots. If such a panel loses power while
AudioX runs, reboot to bring it back.

## Troubleshooting

- **WebUI works, display is blank:** check the Sleep state, panel power/cabling,
  and overlay support. The runtime log shows `[FBUI] KMS display ... initialized`
  for the display in use and `[MODULES] loaded ...` for each driver loaded for
  detected hardware; the kernel log (`/api/rootfs/kernel.log`) shows driver
  probe errors.
- **Touch does not follow the screen:** verify compatible touch input and
  calibration/orientation at the driver/overlay level; there is no general
  calibration wizard on the WebUI.
- **Pads make no sound:** check the soundboard route, output volume, and cached
  clip. The touch display does not automatically connect an output.
- **A hold stops when another held pad is used:** there is one shared held
  voice across playback surfaces.

Use the [soundboard guide](./soundboard) for file management and
[System logs](./system) for diagnostics.
