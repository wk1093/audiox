# AudioX

`audiox` is a custom Raspberry Pi audio workstation project that combines a low-level initramfs software stack with a long-term plan for purpose-built audio hardware.

Right now it boots through a tiny custom bootloader initramfs into a separate runtime initramfs, brings up a USB UAC2 gadget, plays local WAV voices, routes audio with real-time metering, and renders touchscreen + MIDI control with a responsive framebuffer UI; long term the same software will drive multiple reverse-HAT board variants with patchable analog/digital routing, effects, and persistent user configuration.

Warning: This is very work-in-progress and not yet stable. `make dev` builds and uploads the matching kernel, bootloader, and runtime image, but the bootloader is very simple and does not have a recovery mode. If an update breaks boot, you may need to reflash the SD card.

## Contributing

I would really appreciate help on this project. AudioX is currently maintained by one person and has grown to a pretty large codebase.

If you want to help, please start here: [CONTRIBUTING.md](CONTRIBUTING.md)

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
- Accepts complete update archives with `PUT /api/update`; the device validates and unpacks the archive, checks that it was built for this board, installs the kernel under the file name recorded in the archive, the bootloader, and then `config.txt`, then stages `program.cpio.gz` and reboots
- Streams boot-partition files over HTTP:
	- `GET`/`PUT /api/boot/loader` reads or replaces `/boot/initramfs.cpio.gz`
	- `GET`/`PUT /api/boot/kernel` reads or replaces the kernel; `?name=kernel_2712.img` selects the file (`kernel*.img`), otherwise the kernel named by `kernel=` in `/boot/config.txt` (default `kernel8.img`)
	- Boot uploads are limited to 150 MiB and replace the destination only after a complete upload
- Exposes basic rootfs file APIs over HTTP:
	- `GET /api/rootfs/<path>` -> reads `/audiox/<path>`
	- `PUT /api/rootfs/<path>` -> writes `/audiox/<path>`
	- `DELETE /api/rootfs/<path>` -> deletes `/audiox/<path>`
- Exposes system control endpoints:
	- `GET /api/system/info` - returns device info (version, kernel, uptime, memory, load average)
	- `POST /api/system/sync` - flushes filesystem
	- `POST /api/system/restart` - reboots
	- `POST /api/system/shutdown` - powers off
	- `GET`/`DELETE /api/system/boot-failure` - reads or dismisses the last automatic-revert report (`204` when there is none)
- Automatically reverts updates that fail to boot (see [Update rollback](#update-rollback))
- Exposes a web soundboard with per-card MIDI assignment and automatic polling
- Supports MIDI note-to-SFX mappings stored in `/audiox/midi_map.txt` and targeting files in `/audiox/sfx/`
- Removing a sound clears its MIDI note assignments, per-sound lights, and playback-mode overrides and refreshes controller LEDs. Startup and soundboard reload also purge references to missing files.
- Preprocesses uploaded soundboard files under `/audiox/sfx/` through `ffmpeg` into 48kHz stereo PCM16 WAV so MP3/OGG/etc work with the current WAV-only loader
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
- `make upload_bootloader` - build and upload `out/initramfs.cpio.gz` to `/api/boot/loader`
- `make upload_kernel` - stage firmware and upload the configured kernel (for example `out/bootfiles/kernel8.img`) to `/api/boot/kernel?name=<kernel>`
- `make dev` - sync firmware, build matching kernel modules and images, upload kernel and bootloader, then upload the runtime to trigger installation and reboot
- `make update_archive` - build `out/audiox-update.axu` containing build metadata, boot config, bootloader, kernel, and runtime
- `make upload_update` - build the update archive and upload it to `/api/update`; devices running an older runtime need one `make dev` first
- The System page can also upload an `.axu` update image
- `make qemu` - boot in QEMU (aarch64) // doesn't work because qemu raspi support is very minimal (no gui, no USB, no audio)
- `make fancyexport` - wait for SD mount and export build artifacts
- `make image` - create a flashable image file (WARNING: untested, may not work)

The standalone bootloader and kernel upload targets directly replace files on the device's boot partition and do not reboot it. `make dev` performs these uploads in order before staging the runtime update.

Update archives (`.axu`) use a little-endian binary format: an 8-byte `AXUPDATE` magic, a `u32` version (`2`), and a `u32` entry count (`5`), followed by the entries in any order. Each entry is a `u32 type`, `u32 crc32` and `u64 size`, followed by its payload. Entry types: `1` is `config.txt`, `2` is `initramfs.cpio.gz`, `3` is the kernel, `4` is `program.cpio.gz`, and `5` is metadata. Metadata is `key=value` text. `board` (`pi4` or `pi5`) and `kernel` (the boot partition file name, matching `kernel*.img`) are required; `version` and `kernel_version` are optional, and unknown keys are ignored. The device stores the complete archive and validates every entry before replacing any boot file. It rejects archives built for a different board (detected from the device-tree SoC), and archives whose `config.txt` `kernel=` line doesn't name the archive kernel. It writes the kernel first and `config.txt` last, so the firmware only switches kernels once the new file is in place. Version `1` archives (four entries, no metadata) are still accepted; their kernel is installed as `kernel8.img`.

### Desktop audio engine bench

Build and run the host audio testbench with `make audio-bench` and `make run-audio-bench`. It compiles the shared effect and graph-block processors without the Raspberry Pi runtime, plays a 440 Hz test tone through the system's default ALSA output, and serves a live effect control page at `http://127.0.0.1:8765`. To use a WAV source, run `make run-audio-bench BENCH_INPUT=/path/to/audio.wav`; to test headlessly, use `make run-audio-bench BENCH_ALSA_DEVICE=null`. Effect types, bypass, and parameters are changed live from the page. Playback prefers 32-bit PCM for quiet-signal resolution and falls back to 16-bit when the output does not support it. `make test-audio-engine` exercises the shared block graph, PCM conversion, and effect controls.

The Cut effect passes audio when bypassed and outputs silence when enabled. Its `Channels` setting changes matching input/output port counts; since this changes routing topology, it is not MIDI-CC-mappable, but its mute state can be mapped to a MIDI note or toggled from the Web UI. Like other effects, its instance, mute state, and channel count persist across reboots.

Run `make test-soundboard-config` to check Cut persistence and missing-sound MIDI cleanup, including controller LED output. This host test requires ALSA development files and uses an isolated temporary configuration directory.

The Pi runtime defaults to the direct graph-processing loop (`AUDIOX_USE_SHARED_GRAPH_PROCESSOR=0`) to avoid per-block adapter dispatch, stack graph-node setup, and shared processor code in the firmware binary. Set `AUDIOX_USE_SHARED_GRAPH_PROCESSOR=1` to compile the callback-based graph path for target-side comparison; the desktop engine uses the shared path. `processing.o` is rebuilt when the mode changes. Capture stream ownership, device attachment/remapping, and source-specific ring/SRC state live in a separate fixed-capacity ALSA capture backend. PCM I/O remains on the direct Pi path.

### Build configuration

Use `make menuconfig` to edit `.config`. Configuration targets:

- `make defconfig` / `make pi4_defconfig` - regenerate `.config` with the Raspberry Pi 4 defaults
- `make pi5_defconfig` - regenerate `.config` with the Raspberry Pi 5 defaults (untested on hardware)
- `make olddefconfig` - keep existing choices and fill in defaults for new options (also run automatically when `Kconfig` changes)

`Kconfig` is the single source of truth. The board is a `choice` (`BOARD_PI4` / `BOARD_PI5`), and per-board defaults use standard `default VALUE if BOARD_PI5` lines, where the first matching default wins. A non-standard `values` keyword lists the allowed values of a string or int option, and menuconfig shows these as a selector. In menuconfig, Left/Right cycles values, Enter edits, `R` resets to the selected board's defaults, and `s` saves. Changing the board also moves every option that still has its old default to the new board's default. Options covered:

- Board: kernel image (`kernel8.img`, `kernel_2712.img`, `kernel8_rt.img`), device-tree blobs, GPU firmware files, and the USB gadget controller
- Boot: display and DSI overlays, `start_cd`, mini-UART Bluetooth, `arm_boost`, and the kernel command line
- Firmware: the Raspberry Pi firmware git ref, Bluetooth firmware name, and SHA-256
- Network: device IP (also the default `PI_HOST` for upload targets)
- Audio and development options

`config.txt` and `cmdline.txt` are generated from these options as `out/dev-config.txt` and `out/dev-cmdline.txt`, and shared by `make export`, `make boot_config`, update archives, and `make image`. The selected kernel keeps its firmware file name on the boot partition, and `config.txt` points `kernel=` at it. Update archives record the board and kernel name in their metadata, and `make upload_kernel` and `make dev` pass the name to `/api/boot/kernel`. After switching boards, run `make fetch_deps` to restage `out/bootfiles` before building.

The build-time audio engine sample rate is set with `CONFIG_SAMPLE_RATE` in `.config`; rebuild the runtime after changing it. The Web UI does not expose the runtime USB sample-rate field, but preserves its value when saving the other `/audiox/config.txt` settings.

At boot, `/init` is a small PID 1 watchdog that launches the application from `/sbin/audiox-main`. The app reports a heartbeat once per second from its main loop; if it exits or stops heartbeating, the watchdog restarts it with capped backoff. Startup has a two-minute grace period. The realtime audio thread is not involved in heartbeat reporting.

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

### Optional ffmpeg binary for soundboard preprocessing

If you want non-WAV soundboard uploads (mp3/ogg/flac/etc) to auto-convert on the device, provide an aarch64 ffmpeg binary.

Place it at:

```bash
third_party/ffmpeg/ffmpeg
```

Runtime uses ffmpeg from `/audiox/ffmpeg` (storage partition), and `make image` stages it into partition 2 as `/ffmpeg`.

When `third_party/ffmpeg/ffmpeg` is missing, the `ffmpeg` target will auto-download from `FFMPEG_URL` and verify the archive against `FFMPEG_ARCHIVE_SHA256` before extracting.

WARNING: This auto-download is not guaranteed to be stable, and may break if the upstream changes. I also have not done enough legal research to know if I am allowed to keep the url and checksum in this repo, so it may be removed in the future. If you want to use this feature, please download the binary yourself and place it in `third_party/ffmpeg/ffmpeg` or provide your own URL and checksum.

You can override the source path:

```bash
make FFMPEG_BIN=/absolute/path/to/your/ffmpeg image
```

You can also override the download URL and checksum:

```bash
make \
	FFMPEG_URL='https://your-source.example/ffmpeg-aarch64-static.tar.xz' \
	FFMPEG_ARCHIVE_SHA256='<sha256>' \
	ffmpeg
```

Notes:

- If the download is an archive, Make extracts it and picks the first `ffmpeg` binary found.
- If the download is a raw binary file, Make copies it directly.

If your ffmpeg binary is dynamically linked (not fully static), the build now auto-stages required runtime libs into the image from a cross-lib directory.

Defaults:

- `FFMPEG_CROSS_LIB_DIR=/usr/aarch64-linux-gnu/lib`
- `FFMPEG_RUNTIME_LIBS=ld-linux-aarch64.so.1 libc.so.6 libm.so.6 libdl.so.2 librt.so.1 libpthread.so.0 libgcc_s.so.1`

Override example:

```bash
make FFMPEG_CROSS_LIB_DIR=/path/to/aarch64/sysroot/lib initramfs
```

## Update rollback

Every update path (`/api/update`, `/api/initram`, `/api/boot/loader`, `/api/boot/kernel`, `/api/boot/config.txt`, and therefore `make dev` and `make upload_update`) is protected by an automatic rollback. State lives on the config partition under `/audiox/system/`:

1. Before a running image changes any boot file, it copies the current `config.txt`, `initramfs.cpio.gz`, the kernel that `config.txt` boots (for example `kernel8.img`), and `program.cpio.gz` plus its `/etc/audiox-version` into `boot-backup/`, then writes `boot-trial` (`attempts=0`). If a backup already exists it is kept, because it is the last confirmed-good set.
2. On each later boot (after any staged runtime install), the bootloader increments `attempts` and tells the PID 1 watchdog it is a trial boot.
3. During a trial boot the watchdog reboots instead of restarting the runtime when it exits, crashes, or misses its heartbeat, and appends the reason to `boot-trial.log`. Kernel panics reboot through `panic=10` on the kernel command line.
4. Once the runtime has stayed healthy for 60 seconds, it deletes `boot-trial`, `boot-trial.log`, and `boot-backup/`, so a later one-off failure cannot revert a confirmed image.
5. If a boot starts with 3 attempts already used, the bootloader writes `last-boot-failure.txt` (failed and restored versions, attempts, restored files, trial events, and the tail of `stdout.log`/`stderr.log`), copies the backup back to `/boot`, clears the trial state, and reboots.

The web UI shows a persistent banner while `last-boot-failure.txt` exists; dismissing it deletes the report.

Limitations: rollback relies on the new bootloader and kernel getting far enough to run the bootloader and to reboot. A kernel or bootloader that hangs before that cannot recover itself. `panic=10` is only written by `make export` and `make image`, so existing cards need it added to `cmdline.txt` by hand. The first update to a firmware with rollback support is not protected, because the old runtime does not create a backup.

## Notes

- Module seeds live in `depmod.txt`: use `base:` for early boot/UI-critical modules and `normal:` (or no prefix) for the rest
- Bootloader-only storage/filesystem module seeds live in `bootmod.txt`
- Default Pi 4 boot graphics stay on FKMS (`vc4-fkms-v3d-pi4`; the Pi 5 defaults to `vc4-kms-v3d-pi5` because it has no FKMS); framebuffer rendering stages into an off-screen buffer before presenting to reduce visual tearing without switching the display stack
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
