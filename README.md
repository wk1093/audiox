# AudioX

AudioX turns a Raspberry Pi into a configurable audio workstation with USB audio,
flexible routing, effects, a soundboard, MIDI controls, and named Lua automations.
Control it from a browser, a MIDI controller, or a supported touchscreen.

**AudioX is under active development and is not yet stable.** Raspberry Pi 4
Model B is the current hardware target; Pi 5 images and build presets are
available but remain untested on hardware. Back up your setup before updating.
Automatic update reversion has early-boot limitations, so keep a card reader
and a known-working image available.

## Get started

1. [Download AudioX](https://wklab.net/audiox/#download) for your board.
2. Follow the [installation guide](https://wklab.net/audiox/docs/guide/getting-started)
   to flash an SD card and connect to the device.
3. Open `http://audiox.local/` or the default USB address `http://172.31.73.1/`.
4. Create your first audio path using the
   [routing guide](https://wklab.net/audiox/docs/guide/routing).

Flashing erases the selected drive. The device's HTTP control server is intended
for trusted local connections; do not expose it directly to the internet.

## Features

- **Audio routing:** connect input channels, processing nodes, and outputs.
- **Effects:** gain, distortion, pitch, reverb, noise gate, and multi-channel cut,
  with MIDI parameter and bypass controls.
- **Soundboard:** upload short clips, choose one-shot or hold playback, assign
  MIDI pads, and share the saved order with the touchscreen.
- **MIDI and Bluetooth:** controller actions and lighting, optional one-way USB
  MIDI passthrough, and Bluetooth audio with the documented device/codec limits.
- **Named automations:** run keyboard shortcuts or bounded Lua scripts from the
  WebUI or MIDI. Scripts can control effects and their parameters, volumes,
  sounds, keyboard output, and controller MIDI/lighting.
- **Device management:** persistent configuration, setup backups/restores,
  logs, firmware updates, and failed-update reports.

AudioX uses a custom C++ audio engine and a two-stage initramfs system, rather
than a conventional Raspberry Pi OS installation with PipeWire or JACK.

## Documentation

The [documentation site](https://wklab.net/audiox/docs/) is the main guide for
setup, operation, and development. Its Markdown source lives in [docs/](docs/).

| Topic | Guide |
| --- | --- |
| Installation, hardware, and networking | [Getting started](docs/guide/getting-started.md) |
| Every WebUI page | [WebUI overview](docs/guide/web-interface.md) |
| Signal paths, channels, and processing | [Routing](docs/guide/routing.md), [Effects](docs/guide/effects.md), [Volume](docs/guide/volume.md) |
| Clips, controllers, and wireless audio | [Soundboard](docs/guide/soundboard.md), [MIDI](docs/guide/midi.md), [Bluetooth](docs/guide/bluetooth.md) |
| Automation and scripting | [Macros](docs/guide/macros.md), [Lua API](docs/lua/api.md), [Cookbook](docs/lua/cookbook.md) |
| Backups, updates, and diagnostics | [Config](docs/guide/config.md), [System](docs/guide/system.md), [Troubleshooting](docs/guide/troubleshooting.md) |
| Builds, tests, and publication | [Development](docs/guide/development.md), [Publishing](docs/guide/publishing.md) |
| Persistent files and integrations | [Files and HTTP reference](docs/guide/reference.md) |

## Development

Use a Linux host with the AArch64 cross toolchain and the dependencies listed in
the [build guide](docs/guide/development.md).

```sh
make pi4_defconfig
make menuconfig
make initramfs
```

The outputs are `out/initramfs.cpio.gz` (bootloader) and `out/program.cpio.gz`
(runtime). `make image` creates an SD image; `make update_archive` creates an
AXU firmware archive. These build commands do not install them on a device.
Upload/export targets modify real devices or cards: read the build guide before
using `make dev`, `make upload_update`, or export targets.

Tests and audio-benchmark rules live in [tests/Makefile](tests/Makefile), included
by the root Makefile. Run the relevant targets from the repository root.
QEMU debugging and the desktop audio bench are described in the build guide;
neither replaces physical audio/hardware validation.

To build the documentation separately:

```sh
npm ci --prefix docs
npm run docs:build --prefix docs
```

## Contributing and roadmap

Help with reliability, hardware testing, focused features, and documentation is
welcome. Read [CONTRIBUTING.md](CONTRIBUTING.md) and the [roadmap](TODO.md) before
starting a larger change. Report issues with the Pi model, firmware/kernel
version, connected hardware, reproduction steps, and relevant logs.

The long-term goal is a hardware-first workstation with purpose-built audio
boards, additional analog/digital I/O, and patchable signal paths. Those hardware
plans are not capabilities of the current Raspberry Pi images.
