# Getting started

AudioX is a small, purpose-built Raspberry Pi audio system, not an application installed on Raspberry Pi OS. Its image boots directly into an audio engine, a local framebuffer interface, and a browser-based control interface. There is no general-purpose desktop setup wizard.

The project is work in progress. Raspberry Pi 4 Model B is the current hardware target; Pi 5 build defaults and downloads exist but are explicitly **untested on hardware**. A published image or successful build does not establish that a board, peripheral, or host OS has been validated. Read [Hardware and cabling](/guide/hardware) before powering a new setup.

## What you need

- A board matching the download: Pi 4 Model B, or a Pi 5 for experimental use.
- A microSD card and a computer/card reader that can write it. Flashing erases the selected card.
- A reliable, data-capable USB cable to the Pi's USB-C device connection, and a power arrangement suitable for the board and attached peripherals. A charging-only cable will not carry audio or the control network.
- A computer with a browser, a USB NCM network driver, and a DHCP client for that adapter.
- Optionally, a display/touchscreen, USB audio devices, and a MIDI controller. A display is not required for browser control.

Do not assume the computer's USB port can power the Pi and all connected devices. See [power and port roles](/guide/hardware#power-and-port-roles). Internet access is needed on the computer to download releases; the Pi does not need internet for USB control.

## Choose an installation method

| File or method | Purpose | Effect on saved data |
| --- | --- | --- |
| Board-specific `.img.xz` SD image | First installation, clean reinstall, or recovery when the device cannot boot | Repartitions/replaces the selected card; back up first |
| Board-specific `.axu` update archive | Update a booting AudioX device from **System** | Replaces boot/runtime components, rather than formatting the storage partition |
| `program.cpio.gz` or separate kernel/loader files | Developer workflows with matching components | Not an SD image; mismatched components can prevent boot |
| `.axb` backup | Restore user configuration and sound library | Not firmware; restore instructions are on [Config](/guide/config) |

An AXU is not something to flash with an imaging tool. Conversely, do not upload an SD image to the firmware update control.

### Stable or Nightly?

The [download page](https://wklab.net/audiox/#download) separates Stable and Nightly releases and offers Pi 4 and Pi 5 files. Stable is the release channel; Nightly tracks development and may be unstable. A Stable label does not override the Pi 5 hardware warning or the project's overall development status.

Use the board-matched image, AXU, and checksum from the **same release**. Versioned release downloads are preferable when recording a reproducible installation. Nightly history is retained for the latest 25 versioned builds; older nightly links can expire. Stable releases are not pruned by that policy.

## Install with Raspberry Pi Imager

1. If this card already contains AudioX, open **Config > Configuration Backup**, save pending changes, and download a backup to another computer. Confirm that the download finished before proceeding. Follow [Config and backups](/guide/config), including its exclusions; a configuration backup is not a complete card image.
2. Obtain Raspberry Pi Imager. Load the project's custom catalog:

   ```text
   https://raw.githubusercontent.com/wk1093/audiox/main/os_list.json
   ```

   Imager versions differ in how a custom repository is opened. On versions with the repository command-line option, launch:

   ```bash
   rpi-imager --repo https://raw.githubusercontent.com/wk1093/audiox/main/os_list.json
   ```

3. Select the matching Raspberry Pi model, **AudioX Stable** or **AudioX Nightly**, and the matching board entry. Refresh an old catalog if a retained nightly has expired.
4. Select the microSD card. Check its capacity and identity against the physical card, not merely a remembered drive letter. Disconnect unrelated removable disks if that helps avoid mistakes.
5. Review the erase warning and write the image. Let Imager finish writing and verifying. Do not remove the card, reader, or computer power during this process.
6. Safely eject the card.

AudioX does not implement Raspberry Pi OS's first-boot Wi-Fi/SSH/user customization workflow. Do not rely on Imager OS customization to create a wireless connection or login.

## Install a downloaded image

If you prefer a direct download or the custom catalog is unavailable:

1. Download the board-matched `.img.xz` and its `.sha256` sidecar from the download page or the same GitHub release.
2. Verify the **compressed download**, before extracting or flashing. For a Stable Pi 4 image with the published filenames:

   ```bash
   sha256sum -c audiox-latest-pi4.img.xz.sha256
   ```

   Run this in the directory containing both files. Use the actual Pi 5 or Nightly filename when appropriate. On systems without `sha256sum`, calculate SHA-256 using the OS's equivalent tool and compare the entire hash with the sidecar.
3. Stop if the checksum fails. Re-download from the intended release; do not ignore the mismatch. The image catalog also records a separate checksum for the extracted image, so do not compare a compressed file against the extracted-image hash.
4. In Imager's **Use custom** flow, choose the downloaded image. If your imaging tool cannot read `.xz`, decompress it first:

   ```bash
   xz -dk audiox-latest-pi4.img.xz
   ```

5. Select the correct card, write, verify, and safely eject as above.

Checksums detect a corrupted or wrong download. They are not a firmware signature or proof that the publisher's account was uncompromised.

::: danger Reflashing is destructive
Writing an SD image destroys the selected disk's partition layout and saved AudioX configuration/sounds. Do not experiment with raw disk-writing commands unless you can positively identify the target. An update rollback backup on that same card will not survive reflashing.
:::

## First boot and connection

1. With the Pi off, insert the prepared card. Connect optional display/touch hardware according to its board-specific instructions.
2. Connect the Pi's USB-C device port to the computer using a data cable. Arrange reliable power before relying on USB audio or performing updates.
3. Allow the device to boot. It loads a small bootloader, then the runtime; the local display may show boot status if available. There is no fixed first-boot timing guarantee.
4. Check that the host discovers an AudioX USB audio device and an NCM network adapter. Enable **Automatic (DHCP)** on that network adapter.
5. Open **`http://audiox.local/`**, including `http://`. If name resolution fails, try **`http://172.31.73.1/`** after the adapter has an address.
6. Open **System**. Record the reported version, kernel, board model, and uptime. Resolve any automatic-revert banner before installing another update.

The default USB network is `172.31.73.0/24`; AudioX is `172.31.73.1` and assigns the host a DHCP lease. No router/default-gateway option is advertised. Your computer can keep its normal internet connection. If enumeration, DHCP, or names fail, follow [Networking](/guide/networking), not Raspberry Pi OS wireless instructions.

::: warning Use a trusted connection
The current server uses plain HTTP on port 80 without a login/authentication layer. Its API can change files, update firmware, restart, and shut down the device. Keep it on a trusted direct connection; do not forward its port or expose it to an untrusted LAN or the internet.
:::

## Make the first audio connection

Start with monitoring volumes low and avoid feedback loops. Discovering a USB device does not by itself create the desired audio path.

1. Open [Routing](/guide/routing). Identify the host-facing USB endpoints and any attached ALSA devices.
2. Create only the path you need, and inspect its meters before increasing levels.
3. Use [Volume](/guide/volume) for device-side gains. Host mute/volume is independent of Web/MIDI gain; check both when a USB route is silent.
4. Add [Effects](/guide/effects) after a simple route is working.
5. For clips, use [Soundboard](/guide/soundboard). Non-WAV uploads depend on the FFmpeg binary included in your installation; System reports its availability.

For peripherals and automation, continue with [Bluetooth](/guide/bluetooth), [MIDI](/guide/midi), and [Macros](/guide/macros). Save persistent settings using the relevant page; a runtime control change is not necessarily a saved configuration.

## Updating, shutting down, and recovering

- Use [System](/guide/system) for a board-matched AXU update, logs, restart, shutdown, and automatic-revert reports. Download a [configuration backup](/guide/config) before maintenance.
- **Sync Filesystem** flushes already-written data; it does not save pending browser edits or replace a proper shutdown.
- Use **Shutdown Device**, confirm, and allow the device to halt before removing power or the card. Do not pull power during firmware installation.
- An update has limited automatic boot rollback, not a full rescue environment. Early kernel/bootloader hangs can still require a card reader and reflash.
- If the UI is unreachable or audio is not working, preserve logs where possible and follow [Troubleshooting](/guide/troubleshooting).

## Implementation references

This workflow follows the repository's `README.md` installation/update notes, `os_list.json`, `scripts/package-imager.sh`, `scripts/build_image.sh`, `root/etc/dnsmasq.conf`, and `web/updates.js`. For building your own images rather than installing a release, see [Development](/guide/development).
