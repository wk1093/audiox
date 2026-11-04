# Config and backups

Use **Config** to set USB gadget functions and back up or restore AudioX
settings. Routing, effect, soundboard, and script settings are edited on their
respective pages.

The USB settings, routing, MIDI/effect, volume, and Bluetooth pairing files are
schema-versioned. Older unversioned files are read as the legacy schema and
upgraded when saved. AudioX refuses to overwrite a file declaring a newer
schema version than it supports. Backup creation and restore staging also
reject unsupported schema versions, so an older build cannot silently discard
newer settings.

## USB audio settings

| Field | Meaning |
| --- | --- |
| USB Playback Channels | AudioX-to-host channels, shown as `usb_gadget_out` in routing; the host records this audio |
| USB Capture Channels | Host-to-AudioX channels, shown as `usb_gadget_in` in routing; the host plays into this source |
| USB Sample Size (bytes) | Bytes per sample exposed by the USB audio function |
| USB MIDI Passthrough | Whether to expose USB MIDI, and whether to forward mapped messages |
| USB Keyboard Chords | Whether to expose the combined USB HID keyboard/Consumer Control media-key interface |

Channels range from 1–16 and sample size from 1–4 bytes. Stereo in each
direction with two bytes per sample (16-bit) is a typical starting format.
Select the matching AudioX device and channel count in host software.

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
4. **Recommended:** use **System > Restart Device** to apply the saved USB
   changes with a full reboot. **Reload Config** attempts to apply them without
   rebooting, but is unreliable.

::: warning USB reload can cause instability
Reload Config can cause unpredictable instability or leave the system
unresponsive until rebooted. **Save Config, then restart the device instead.**
The reload confirmation offers **Reboot Instead** to restart the device and
apply saved USB changes without attempting a live reload. Unsaved edits are
not saved automatically.
Reloading disconnects USB **audio, MIDI, keyboard, and networking**, and they
may not recover without a reboot.
Stop playback/recording in host applications first, save work, and be prepared
to reselect the device in your DAW or operating system. If you are connected
over USB networking, the browser may lose contact while the link reconnects.
:::

If Reload rejects a format, read the reported error, return to a known-working
configuration, save, and restart the device. A USB reload is not a whole-device reboot.

The keyboard setting is disabled by default. It is needed only for scripts that
generate keyboard reports; volume/effect/logging scripts do not need it.
See [Scripts](./macros). USB MIDI passthrough is also disabled by default:
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
  script definitions.
- Saved effect settings and volumes.
- Bluetooth pairing data.
- Soundboard audio and its saved order.

The backup does **not** contain the installed kernel, bootloader, runtime
firmware, arbitrary files elsewhere on the system, browser-only graph layout,
or Lua's in-memory `state`. Dim-lights state and current playback are not saved.
Script-owned persistent options and custom files under `/audiox/scripts/` are
also excluded; export important script data separately.

::: warning Keep backups private
Bluetooth pairing records contain keys. A backup can also include private audio
clips and script text. Store and share it as private device data, not a public
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

Before upload, the browser checks for free space equal to twice the archive
size plus 16 MiB of staging overhead. A 44.5 MiB archive therefore requires at
least 105 MiB free. The check runs again when upload starts; a rejection reports
required and available bytes without changing settings.

Before applying a pending restore, AudioX expands the storage partition and its
ext4 filesystem to fill the SD card. The boot partition and existing files are
preserved. The first expansion can delay startup; keep power connected until
boot completes. Later boots leave an expanded filesystem unchanged.

Automatic expansion supports the shipped two-primary-partition MBR layout on
512-byte-sector disks up to the MBR size limit (approximately 2 TiB). Custom GPT,
extra-partition, overlapping, or otherwise inconsistent layouts are left unchanged
with a `[STORAGE]` boot warning. Expansion failures are reported and retried on
the next boot; AudioX can still use the existing storage size.

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

Continue with [System and updates](./system) or the
[persistent-files and HTTP reference](./reference).
