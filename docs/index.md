# AudioX documentation

AudioX routes audio between USB, ALSA, Bluetooth, and soundboard sources,
through effects, and to connected outputs. Operate it from the WebUI, a MIDI
controller, or an optional touchscreen.

AudioX is under active development. Raspberry Pi 4 Model B and Raspberry Pi 5
are hardware-tested; the Pi 5 framebuffer UI has not been tested with a
connected display. Start with [Getting started](./guide/getting-started) for
installation, connection, and a first audio route.

## Set up the device

- [Hardware and compatibility](./guide/hardware)
- [Networking](./guide/networking)
- [WebUI](./guide/web-interface)
- [Display and touch](./guide/display)

## Build an audio setup

| Task | Guide |
| --- | --- |
| Connect sources, processing, and outputs | [Routing](./guide/routing) |
| Recall named routing, gain, and effect inventories | [Setup scenes](./scenes) |
| Pair a phone or Bluetooth output | [Bluetooth](./guide/bluetooth) |
| Add gain, distortion, gate, pitch, reverb, or cut | [Effects](./guide/effects) |
| Set node gain or map a MIDI controller | [Volume](./guide/volume) |
| Upload, play, order, and map audio clips | [Soundboard](./guide/soundboard) |
| Configure controller actions, sampler, or lighting | [MIDI](./guide/midi) |
| Create named shortcuts and automations | [Scripts](./guide/macros) |

## Maintain and troubleshoot

- [Config and backups](./guide/config)
- [System, logs, and updates](./guide/system)
- [Troubleshooting](./guide/troubleshooting)
- [Persistent files and HTTP API](./guide/reference)

## Development

- [Developer workflows](./dev/)
- [Builds and tests](./guide/development)
- [Release publishing](./guide/publishing)
- [Telemetry collector setup](./dev/telemetry)

## Lua scripting

Scripts use event callbacks to run bounded actions, maintain simple in-memory
state, and control sounds, effects, volumes, keyboard output, and a MIDI
controller. Begin with [Lua scripting](./lua/), then use the
[API reference](./lua/api) and [cookbook](./lua/cookbook).

Lua runs on the device. Keyboard actions require an enabled USB keyboard and a
connected host; MIDI and lighting require the selected controller; sound,
effect, and volume actions require the corresponding AudioX target. Scripts
cannot change routing.

## Downloads and source

Download board-specific images and firmware from the
[AudioX website](https://wklab.net/audiox/), or browse the
[source repository](https://github.com/wk1093/audiox).

The device's HTTP control server has no authentication or TLS. Keep it on a
trusted local connection; do not expose it directly to the internet.
