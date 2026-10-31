# AudioX documentation

AudioX turns a Raspberry Pi into a configurable audio workstation: connect
sources to outputs, insert effects, play short clips, and control the setup
from a browser, a MIDI controller, or a supported touchscreen.

These guides describe the current firmware. AudioX is under active development;
older installed versions may not include every control described here.
Raspberry Pi 4 and Pi 5 are hardware-tested targets. The Pi 5 framebuffer UI
remains untested with a connected display; see [Display and touch](./guide/display).

::: tip New device?
Start with [Getting started](./guide/getting-started): choose an image, flash an
SD card, connect to the WebUI, and make your first audible route.
:::

## Set up your device

- [Getting started](./guide/getting-started) - installation and your first session.
- [Hardware and compatibility](./guide/hardware) - Pi models, ports, cables,
  optional devices, and current limitations.
- [Networking](./guide/networking) - USB networking, addressing, name resolution,
  and offline use.
- [WebUI overview](./guide/web-interface) - every page, save behavior, notifications,
  and long-running operations.

## Build your audio workflow

| I want to... | Read |
| --- | --- |
| Connect inputs to outputs and understand channels | [Routing](./guide/routing) |
| Pair a phone or Bluetooth output device | [Bluetooth](./guide/bluetooth) |
| Add gain, distortion, gate, pitch, reverb, or a multi-channel cut | [Effects](./guide/effects) |
| Adjust levels or assign a MIDI knob | [Volume](./guide/volume) |
| Upload, play, order, and map short clips | [Soundboard](./guide/soundboard) |
| Configure controller actions, sampler playback, lights, and USB passthrough | [MIDI](./guide/midi) |
| Run a named shortcut or scripted scene from the WebUI/MIDI | [Macros](./guide/macros) |
| Use local meters and touch pads | [Display and touch](./guide/display) |

## Keep a working setup

- [Config and backups](./guide/config) explains staged USB settings and `.axb`
  backup/restore.
- [System and updates](./guide/system) covers information, logs, power actions,
  firmware installation, and failed-boot recovery.
- [Troubleshooting](./guide/troubleshooting) follows failures from connection
  through the signal path.
- [Files and HTTP reference](./guide/reference) maps persistent data and
  advanced integration endpoints.
- [Building and development](./guide/development) covers custom images,
  configuration, tests, and local docs.
- [Releases and publishing](./guide/publishing) covers maintainer workflows,
  version strings, and Imager catalogs.

## Program automations with Lua

Named macros can run functions, make decisions, maintain bounded runtime state,
and queue keyboard, sound, MIDI, effect, or volume actions.
Start with [Lua scripting](./lua/), then use the
[API reference](./lua/api) and [cookbook](./lua/cookbook).

Lua runs on the device, not on this documentation website. Only keyboard
actions require a USB keyboard connection; MIDI/lighting actions need their
controller, and sound/effect/volume actions need the corresponding AudioX target.
Route-changing scripts are not implemented.

## Downloads and source

Use the [AudioX website](https://wklab.net/audiox/) for board-specific SD images
and firmware archives, or browse the
[source repository](https://github.com/wk1093/audiox).
Keep a backup before updating or reflashing, and use AudioX on a trusted local
connection rather than exposing its device-control server to the internet.
