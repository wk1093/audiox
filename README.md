# AudioX

AudioX is a Raspberry Pi audio workstation with USB audio, a configurable
signal-routing graph, effects, a soundboard, MIDI control, Bluetooth audio, and
named Lua automations. Control it from a browser, a MIDI controller, or a
supported touchscreen.

AudioX is under active development and is not yet stable. Raspberry Pi 4 Model B
and Raspberry Pi 5 are hardware-tested; the Pi 5 framebuffer UI has not been
tested with a connected display. Keep a configuration backup and a known-good
image. Automatic update reversion cannot recover from every early-boot failure.

## Install and connect

1. Download the image for your board from the
   [AudioX website](https://wklab.net/audiox/#download).
2. Flash an SD card using the [installation guide](https://wklab.net/audiox/docs/guide/getting-started).
3. Connect the Pi's USB-C device port to a computer with a data-capable cable.
4. Open `http://audiox.local/` or `http://172.31.73.1/`.
5. Follow the [routing guide](https://wklab.net/audiox/docs/guide/routing) to
   connect an input to an output.

Flashing erases the selected drive. The device serves an unauthenticated,
plain-HTTP control interface; keep it on a trusted local connection and do not
expose it to the internet.

## Capabilities

- Route USB, ALSA, Bluetooth, and soundboard audio through processing nodes.
- Apply gain, distortion, pitch, reverb, noise gate, and multi-channel cut.
- Play short soundboard clips from the browser, MIDI, or local display.
- Map MIDI controls, drive controller lighting, and use one-way MIDI passthrough
  to a USB host.
- Run bounded Lua scripts for sound, effects, volume, MIDI, lighting, and
  keyboard actions.
- Back up and restore settings, inspect logs, and install firmware updates.

AudioX uses a custom C++ audio engine and two-stage initramfs image. It is not a
Raspberry Pi OS application and does not use PipeWire or JACK.

## Guides

The [documentation site](https://wklab.net/audiox/docs/) covers setup, operation,
and development. Markdown sources are in [`docs/`](docs/).

| Topic | Guide |
| --- | --- |
| Installation, hardware, and networking | [Getting started](docs/guide/getting-started.md), [Hardware](docs/guide/hardware.md), [Networking](docs/guide/networking.md) |
| Web control and local display | [WebUI](docs/guide/web-interface.md), [Display and touch](docs/guide/display.md) |
| Signal flow and levels | [Routing](docs/guide/routing.md), [Effects](docs/guide/effects.md), [Volume](docs/guide/volume.md) |
| Clips, MIDI, and Bluetooth | [Soundboard](docs/guide/soundboard.md), [MIDI](docs/guide/midi.md), [Bluetooth](docs/guide/bluetooth.md) |
| Automation | [Scripts](docs/guide/macros.md), [Lua API](docs/lua/api.md), [Cookbook](docs/lua/cookbook.md) |
| Native effect development | [Plugins and AXM format](docs/plugins.md) |
| Backups, updates, and diagnostics | [Config](docs/guide/config.md), [System](docs/guide/system.md), [Troubleshooting](docs/guide/troubleshooting.md) |
| Builds and releases | [Development](docs/guide/development.md), [Publishing](docs/guide/publishing.md) |
| Persistent files and HTTP API | [Reference](docs/guide/reference.md) |

## Build and test

Use Linux with the AArch64 cross toolchain and dependencies in the
[development guide](docs/guide/development.md).

```sh
make pi4_defconfig
make menuconfig
make initramfs
```

The build produces `out/initramfs.cpio.gz` and `out/program.cpio.gz`.
`make image` creates an SD image; `make update_archive` creates an AXU firmware
archive. Build targets create host-side files. `make dev`, `make upload_update`,
and export targets modify a device or card; review their instructions first.

Test targets are defined in `tests/Makefile` and included
from the root Makefile. Run the relevant targets from the repository root.
QEMU and desktop audio tests do not replace physical hardware validation.

To build the documentation:

```sh
npm ci --prefix docs
npm run docs:build --prefix docs
```

## Contributing

Focused contributions, hardware testing, reliability work, and documentation
updates are welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) and [docs/TODO.md](docs/TODO.md)
before starting a larger change. Issue reports are most useful with the Pi
model, firmware and kernel versions, connected hardware, reproduction steps,
and relevant logs.
