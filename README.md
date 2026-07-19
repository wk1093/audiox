# AudioX

`audiox` is a custom Raspberry Pi audio workstation project that combines a low-level initramfs software stack with a long-term plan for purpose-built audio hardware.

Right now it boots through a tiny custom bootloader initramfs into a separate runtime initramfs, brings up a USB UAC2 gadget, plays local WAV voices, routes audio with real-time metering, and renders touchscreen + MIDI control with a responsive framebuffer UI; long term the same software will drive multiple reverse-HAT board variants with patchable analog/digital routing, effects, and persistent user configuration.

Warning: This is very work-in-progress and not yet stable. The `make dev` target will build and upload a new runtime image, but be warned that certain updates require a full reflash (via `make fancyexport` or `make image`) to work properly. The bootloader is very simple and does not have a recovery mode, so if you break the runtime image you will need to reflash the SD card.

Once I believe this is stable enough for general use, I will release v2.0.0

## What it does

- Loads boot-critical modules first from `/etc/module-load.base.list`, then normal modules from `/etc/module-load.normal.list`
- Keeps a compatibility combined list at `/etc/module-load.list`
- Uses a two-stage ramfs layout: `initramfs.cpio.gz` bootloader plus `program.cpio.gz` runtime
- Bootloader stages its own storage/filesystem modules from `bootmod.txt`
- Configures a USB audio gadget through ConfigFS with bidirectional capture/playback
- Configures a USB NCM network gadget (`usb0`) for host<->Pi control traffic
- Dedicates a real-time audio thread (SCHED_FIFO) for low-latency capture/playback processing
- Routes audio between capture devices, effects, and playback sinks with live-metering
- Polls touch + MIDI input and renders a responsive framebuffer UI with CPU/RAM metrics
- Stores config at `/audiox/config.txt`
- Runs a basic HTTP server on port 80 for web tooling
- Serves a static page from `/etc/www/index.html`
- Accepts firmware updates with `PUT /api/initram`, stages a new `program.cpio.gz`, reboots into the bootloader, promotes the staged image to the boot partition, and reboots again
- Exposes basic rootfs file APIs over HTTP:
	- `GET /api/rootfs/<path>` -> reads `/audiox/<path>`
	- `PUT /api/rootfs/<path>` -> writes `/audiox/<path>`
	- `DELETE /api/rootfs/<path>` -> deletes `/audiox/<path>`
- Exposes system control endpoints:
	- `GET /api/system/info` - returns device info (version, kernel, uptime, memory, load average)
	- `POST /api/system/sync` - flushes filesystem
	- `POST /api/system/restart` - reboots
	- `POST /api/system/shutdown` - powers off
- Exposes a web soundboard with per-card MIDI assignment and automatic polling
- Supports MIDI note-to-SFX mappings from config stored in `/audiox/config.txt` and targeting files in `/audiox/sfx/`
- Web UI with 4 main pages: Routing (graph + metrics), Config (audio settings), Soundboard (cards + upload/assign/remove), System (device stats)

## Build

Main output is an initramfs image:

```bash
make initramfs
```

This produces two boot artifacts in `out/`:

- `initramfs.cpio.gz` - tiny bootloader image loaded first
- `program.cpio.gz` - main runtime image loaded second

Useful targets:

- `make show_kernel` - show detected kernel version
- `make show_alsa` - print ALSA dependency mode and resolved paths/flags
- `make alsa` - prepare static `libasound.a` into `out/alsa-sysroot`
- `make dev` - build runtime image, upload it over HTTP, then let the Pi reboot twice to install it
- `make qemu` - boot in QEMU (aarch64) // doesn't work because qemu raspi support is very minimal (no gui, no USB, no audio)
- `make fancyexport` - wait for SD mount and export build artifacts
- `make image` - create a flashable image file (WARNING: untested, may not work)

### ALSA dependency setup on amd64 host

When building with cross compiler on amd64, the runtime now expects ALSA under `out/alsa-sysroot` and links against static `libasound.a`.

Source build is the only supported mode:

```bash
make alsa
make initramfs
```

Useful override:

```bash
# pin upstream alsa-lib source version
make ALSA_VERSION=1.2.14 alsa
```

## Notes

- Module seeds live in `depmod.txt`: use `base:` for early boot/UI-critical modules and `normal:` (or no prefix) for the rest
- Bootloader-only storage/filesystem module seeds live in `bootmod.txt`
- Default boot graphics stay on FKMS (`vc4-fkms-v3d-pi4`); framebuffer rendering stages into an off-screen buffer before presenting to reduce visual tearing without switching the display stack
- v1.1 adds UI revamp with 4-page design (Routing, Config, Soundboard, System), per-card MIDI assignment with automatic polling, system info endpoint, improved Flexbox layout for routing page, and DELETE file API
- v1.0.2 was the previous stable release with real-time audio processing, touchscreen UI, and HTTP control

Network currently requires this on linux host (sometimes, only tested on one machine):
Either this on every time you plugin the Pi:
```bash
sudo ip addr add 169.254.0.1/16 dev usb0
sudo ip link set usb0 up
```

Or this once:
```bash
sudo nmcli connection add type ethernet ifname usb0 con-name audiox-usb0 ipv4.method link-local ipv6.method ignore connection.autoconnect yes
sudo nmcli connection up audiox-usb0
```

Quick endpoint examples (replace IP if needed):
```bash
# static web page
curl http://169.254.1.2/

# upload a new runtime image; the Pi reboots into the bootloader, installs it, then reboots again
curl --fail --show-error -X PUT --data-binary @out/program.cpio.gz http://169.254.1.2/api/initram

# get device info (version, kernel, uptime, memory, load)
curl http://169.254.1.2/api/system/info

# read/write/delete config files via HTTP API
curl http://169.254.1.2/api/rootfs/config.txt
curl -X PUT --data-binary @config.txt http://169.254.1.2/api/rootfs/config.txt
curl -X DELETE http://169.254.1.2/api/rootfs/sfx/myfile.wav

# trigger soundboard slot 0 from web/API
curl http://169.254.1.2/api/soundboard/trigger/0

# write staging audio config and reload (promotes to config.txt on success)
cat <<'EOF' | curl -X PUT --data-binary @- http://169.254.1.2/api/rootfs/config.staging.txt
usb_playback_channels=4
usb_capture_channels=2
usb_sample_rate=48000
usb_sample_size=2
EOF
curl -X POST http://169.254.1.2/api/config/reload
```

MIDI mapping entries in `/audiox/config.txt`:

```txt
usb_sample_rate=48000
usb_playback_channels=2
usb_capture_channels=2
usb_sample_size=2

midi_map=36,kick.wav
midi_map=38,snare.wav
midi_map=42,hats/closed.wav
```

Each `midi_map` is `note_number,sfx_relative_path`.
Relative paths resolve under `/audiox/sfx/`.


## Long-term vision

This is where I want audiox to go:

- A custom reverse-HAT style board where the Raspberry Pi mounts on top of a larger audio board
- At least 4 audio inputs and 4 audio outputs using dedicated ADC/DAC chips
- A 48V mic input and at least 3 instrument inputs with basic op-amp front ends for clean, strong signal levels
- Patchable routing everywhere: for now likely with jumper wires, later possibly full audio jack patch bays
- Dedicated outputs for headphones and monitor speakers
- Built-in mixers and hardware volume knobs, with mixer paths also patchable (analog + digital in any combination)
- Touchscreen control for "virtual audio cables", effects routing, and mix control
- MIDI-driven control over digital effects, including touchscreen MIDI mapping tools
- Soundboard support by uploading audio files into rootfs
- Persistent storage of config + mappings, with a simple text format for MIDI mappings so edits can be made without the touchscreen
- Multiple board variants for different users, all running the same software stack (for example: a compact 4x4 digital + 2x4 analog option, and larger premium options like 8x8 or 4x8)
- A deliberately simple rootfs layout so advanced users can do precise configuration without reflashing initramfs
- If touchscreen flow is not preferred, configs should still be fully manageable through files; later I may add desktop software with a better editor UX than a small Pi touchscreen
- Raspberry Pi compatibility beyond 4B: target 4 and 5 first, with 3 as a best-effort option (likely limited by CPU headroom for full routing/effects workloads)
- Reduce model-specific assumptions in boot/module setup (especially Makefile and hardware init paths) so one codebase can adapt across Pi generations
- Keep the audio engine custom and close to hardware (no PipeWire/JACK), with room to split into multiple custom processes later only if profiling shows it is needed for stable low-latency performance

End goal: a premium, flexible audio workstation/interface that feels like hardware-first patching plus deep digital control.
