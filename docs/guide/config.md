# Config and backups

The **Config** page controls the USB gadget presented to the host computer and
the configuration backup/restore workflow. Audio routing, effect mappings,
soundboard files, and macros have their own editors on other pages.

## USB audio settings

| Field | Meaning |
| --- | --- |
| USB Playback Channels | AudioX-to-host channels, shown as `usb_gadget_out` in routing; the host records this audio |
| USB Capture Channels | Host-to-AudioX channels, shown as `usb_gadget_in` in routing; the host plays into this source |
| USB Sample Size (bytes) | Bytes per sample exposed by the USB audio function |
| USB MIDI Passthrough | Whether to expose USB MIDI, and whether to forward mapped messages |
| USB Keyboard Chords | Whether to expose the optional USB HID keyboard function |

The channel controls offer 1-16; sample size offers 1-4 bytes. The usual starting
point is stereo in both directions and two bytes per sample (16-bit). Leave a
working format unchanged unless your host or signal path requires something else.
The host software must select the matching AudioX input/output and channel count.

These form labels follow the gadget's ALSA direction, **not the host application's
playback/recording labels**. A host playback stream enters AudioX as
`usb_gadget_in`; `usb_gadget_out` is a destination the host can record.
Use these IDs and node input/output ports when tracing a route.
See the [routing guide](./routing).

Sample rate is retained in the configuration, but the current form does not
offer a sample-rate selector. The engine sample rate and processing buffer size
also have build-time settings. Editing a USB descriptor alone is not a way to
retune the entire engine; see [development](./development).

## Load, Save, and Reload are different

1. **Load Config** fills the form from the staged configuration if available,
   otherwise from the active configuration.
2. Edit the fields.
3. **Save Config** writes the staged configuration. It does not by itself
   recreate the USB device.
4. **Reload Config** applies the staged configuration and reloads the gadget.

::: warning USB reconnect
Reloading briefly disconnects USB **audio, MIDI, keyboard, and networking**.
Stop playback/recording in host applications first, save work, and be prepared
to reselect the device in your DAW or operating system. If you are connected
over USB networking, the browser may lose contact while the link reconnects.
:::

If Reload rejects a format, read the reported error, return to a known-working
configuration, save, and reload again. A USB reload is not a whole-device reboot.

The keyboard setting is disabled by default. It is needed only for macros that
generate keyboard reports; volume/effect/logging scripts do not need it.
See [Macros](./macros). USB MIDI passthrough is also disabled by default:
[MIDI forwarding modes](./midi#forward-midi-to-a-usb-host).

## Back up your setup

Before reinstalling, updating, or making a major change:

1. Save routing and any open editors. Save staged USB changes intentionally;
   the backup includes staged configuration too.
2. Open **Config > Configuration Backup**.
3. Choose **Backup Configuration**.
4. Wait for the browser to finish downloading `audiox-backup.axb`.
5. Keep a second copy somewhere other than the Pi's SD card.

The download uses the browser's normal download mechanism. A "download requested"
notification means the request was opened, not that a complete archive has
already arrived. Check the browser's download list. Server errors may appear
in the newly opened download tab.

### What a backup includes

- Active and staged AudioX configuration.
- Routing.
- MIDI sound/action mappings, controller lighting, sampler settings, and named
  macro definitions.
- Saved effect settings and volumes.
- Bluetooth pairing data.
- Soundboard audio and its saved order.

The backup does **not** contain the installed kernel, bootloader, runtime
firmware, arbitrary files elsewhere on the system, browser-only graph layout,
or Lua's in-memory `state`. Dim-lights state and current playback are not saved.

::: warning Keep backups private
Bluetooth pairing records contain keys. A backup can also include private audio
clips and macro text. Store and share it as private device data, not a public
bug-report attachment.
:::

## Restore a backup

1. Install a working, compatible AudioX image if recovering from a reflash.
2. Make a backup of anything currently on the destination device that you want
   to keep.
3. Select the `.axb` under **Backup Archive**.
4. Choose **Restore Configuration**.
5. Read the replacement warning and confirm **Restore and reboot**.
6. Keep the page open while the archive uploads and is validated.
7. Wait for the new boot, then check routing, devices, sounds, and mappings.

Restore is **replacement, not merge**. Supported settings and sounds absent
from the backup are removed from the restored setup. Firmware and boot files
are not replaced.

The maximum archive size is **4 GiB**. The device needs enough free storage for
the upload and extracted/staged contents, not merely the compressed-looking file
size in the browser. The `.axb` archive is an AudioX backup format, not a ZIP
file; changing another archive's extension will not make it compatible.

The backend validates the archive and stages the restore before reboot.
Incomplete or rejected archives should leave the current setup unchanged.
After a lost connection, the restore may already have been staged: check the
device before retrying instead of repeatedly submitting it.

## Do not confuse backup and firmware files

| File | Purpose | Where to use it |
| --- | --- | --- |
| `.axb` | Saved settings, pairings, and soundboard files | Config > Restore Configuration |
| `.axu` | Board-specific firmware update | System > Firmware Update |
| `.img.xz` | Full SD-card image | An SD-card flashing tool; erases the selected drive |

Continue with [System and updates](./system) or [persistent files and HTTP reference](./reference).
