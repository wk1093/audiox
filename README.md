# audiox

`audiox` is a custom Raspberry Pi audio workstation project that combines a low-level initramfs software stack with a long-term plan for purpose-built audio hardware.

Right now it boots straight into a custom `init` process, brings up a USB UAC2 gadget, plays local WAV voices, and renders touchscreen + MIDI control; long term the same software will drive multiple reverse-HAT board variants with patchable analog/digital routing, effects, and persistent user configuration.

## What it does

- Loads the required kernel modules from `/etc/module-load.list`
- Configures a USB audio gadget through ConfigFS
- Configures a USB NCM network gadget (`usb0`) for host<->Pi control traffic
- Starts audio playback in a dedicated audio thread
- Polls touch + MIDI input and renders a framebuffer UI
- Stores a simple config flag at `/audiox/config.txt`
- Runs a basic HTTP server on port 80 for early web tooling
- Serves a static page from `/etc/www/index.html`
- Exposes basic rootfs file APIs over HTTP:
	- `GET /main/rootfs/<path>` -> reads `/audiox/<path>`
	- `PUT /main/rootfs/<path>` -> writes `/audiox/<path>`
- Exposes a web soundboard trigger endpoint so web and MIDI can both trigger voices

## Build

Main output is an initramfs image:

```bash
make initramfs
```

Useful targets:

- `make show_kernel` - show detected kernel version
- `make qemu` - boot in QEMU (aarch64) // doesn't work because qemu raspi support is very minimal (no gui, no USB, no audio)
- `make fancyexport` - wait for SD mount and export build artifacts
- `make image` - create a flashable image file
- `make debug` - native x86_64 debug boot in QEMU // probably doesn't work anymore because of raspi-specific code

## Notes

- Put `voice0.wav` ... `voice3.wav` in `wavs/`
- This project is still rough around the edges, but it is usable for bring-up and iteration

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

# read/write config files via HTTP API
curl http://169.254.1.2/main/rootfs/config.txt
curl -X PUT --data-binary '1\n' http://169.254.1.2/main/rootfs/config.txt

# trigger soundboard slot 0 from web/API
curl http://169.254.1.2/api/soundboard/trigger/0

# write staging audio config and reload (promotes to config.txt on success)
cat <<'EOF' | curl -X PUT --data-binary @- http://169.254.1.2/api/rootfs/config.staging.txt
usb_playback_channels=4
usb_capture_channels=2
usb_sample_rate=44100
usb_sample_size=2
EOF
curl -X POST http://169.254.1.2/api/config/reload
```


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
