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
- Configures a USB NCM network gadget (`usb0`) for host<->Pi control traffic, with DHCP, DNS, and mDNS so the host can open `http://audiox.local/`
- Dedicates a real-time audio thread (SCHED_FIFO) for low-latency capture/playback processing
- Routes audio between capture devices, effects, and playback sinks with live-metering
- Polls touch + MIDI input and renders a responsive framebuffer UI with CPU/RAM metrics
- Stores config at `/audiox/config.txt`
- Runs a basic HTTP server on port 80 for web tooling
- Serves a static page from `/etc/www/index.html`
- Accepts firmware updates with `PUT /api/initram`, stages a new `program.cpio.gz`, reboots into the bootloader, promotes the staged image to the boot partition, and reboots again
- Accepts complete update archives with `PUT /api/update`; the device validates and unpacks the archive, checks that it was built for this board, installs the kernel under the file name recorded in the archive, the bootloader, `cmdline.txt`, and then `config.txt`, then stages `program.cpio.gz` and reboots
- Streams boot-partition files over HTTP:
	- `GET`/`PUT /api/boot/loader` reads or replaces `/boot/initramfs.cpio.gz`
	- `GET`/`PUT /api/boot/kernel` reads or replaces the kernel; `?name=kernel_2712.img` selects the file (`kernel*.img`), otherwise the kernel named by `kernel=` in `/boot/config.txt` (default `kernel8.img`)
	- `GET`/`PUT /api/boot/cmdline.txt` reads or replaces the kernel command line; it must be a single line containing `rdinit=/init` (needed to boot the AudioX initramfs)
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
- `make upload_cmdline` - upload `out/dev-cmdline.txt` to `/api/boot/cmdline.txt`
- `make dev` - sync firmware, build matching kernel modules and images, upload kernel, bootloader, `cmdline.txt` and `config.txt`, then upload the runtime to trigger installation and reboot
- `make update_archive` - build `out/audiox-update.axu` containing build metadata, boot config, kernel command line, bootloader, kernel, and runtime
- `make upload_update` - build the update archive and upload it to `/api/update`; devices running an older runtime need one `make dev` first
- The System page can upload an `.axu` update image or check GitHub for a board-matched Stable/Nightly release and download/install it through the browser (see [Browser-driven updates](#browser-driven-updates))
- `make qemu` - boot in QEMU (aarch64) // doesn't work because qemu raspi support is very minimal (no gui, no USB, no audio)
- `make debug` - build the QEMU debug variant and boot it in a virtual machine (see [QEMU debug mode](#qemu-debug-mode))
- `make debug_ui` - same as `make debug`, plus a window showing the touchscreen framebuffer
- `make fancyexport` - wait for SD mount and export build artifacts
- `make image` - create a flashable image file (WARNING: untested, may not work)

The standalone bootloader and kernel upload targets directly replace files on the device's boot partition and do not reboot it. `make dev` performs these uploads in order before staging the runtime update.

Update archives (`.axu`) use a little-endian binary format: an 8-byte `AXUPDATE` magic, a `u32` version (`2`), and a `u32` entry count (1 to 64), followed by the entries in any order. Each entry is a `u32 type`, `u32 crc32` and `u64 size`, followed by its payload. Entry types: `1` is `config.txt`, `2` is `initramfs.cpio.gz`, `3` is the kernel, `4` is `program.cpio.gz`, `5` is metadata, and `6` is `cmdline.txt` (optional; when present it must be a single line containing `rdinit=/init`). Metadata is `key=value` text. `board` (`pi4` or `pi5`) and `kernel` (the boot partition file name, matching `kernel*.img`) are required; `version` and `kernel_version` are optional, and unknown keys are ignored. The device stores the complete archive and validates every entry before replacing any boot file. It rejects archives built for a different board (detected from the device-tree SoC), and archives whose `config.txt` `kernel=` line doesn't name the archive kernel. It writes the kernel first and `config.txt` last, so the firmware only switches kernels once the new file is in place. Version `1` archives (four entries, no metadata) are still accepted; their kernel is installed as `kernel8.img`.

The version 2 format is designed so that newer archives keep installing on older v2 devices:

- Devices skip entry types they don't know, after checking their CRC. If a type has bit 31 set (`0x80000000`), the device must understand it, so an older device rejects the archive instead.
- Devices ignore unknown metadata keys.
- Absent optional entries (currently only `cmdline.txt`) leave the device's existing file in place.

To extend the format, add new entry types or metadata keys, and never change the meaning of an existing one. Bump the header version only for changes that can't be expressed this way; older devices then refuse the archive cleanly instead of installing it incorrectly. Devices also cap uploads at 256 MiB, so archives must stay under that. Firmware from before this rule (1.9.0 and earlier) accepts only exactly five entries, so update those devices once with `make dev` or a five-entry archive (packed without `--cmdline`).

### QEMU debug mode

`make debug` builds the firmware with `AUDIOX_QEMU=1` into `out/qemu/` and boots it on a generic `virt` arm64 machine in this terminal (serial console; quit with `Ctrl-A X`). Only the device-specific parts change: the boot and storage partitions are `/dev/vda1` and `/dev/vda2` on a virtio disk instead of `/dev/mmcblk0p1`/`p2`, the USB gadget binds to the `usbip-vudc.0` virtual device controller instead of the dwc2 UDC (`fe980000.usb`), and fatal errors go to `/dev/console` instead of `/dev/tty1`. The normal build (`AUDIOX_QEMU=0`, the default) is unchanged. `include/defs.hpp` derives these from `AUDIOX_QEMU`; `BOOT_DEVICE_PATH` and `CONFIG_DEVICE_PATH` can also be overridden with `-D` flags.

The Raspberry Pi kernel has no virtio drivers, so the VM runs the Debian arm64 kernel pinned by `QEMU_KERNEL_VERSION` (checksum-verified, extracted to `out/qemu-kernel/`), and modules are resolved from it. Seeds in `depmod.txt` and `bootmod.txt` use the `qemu` board filter, for example `base(!qemu):` for Pi-only hardware drivers. Toolchain dependencies (ALSA, SBC, dnsmasq, downloads) are shared with the normal build in `out/`.

Once booted:

- Web UI: <http://127.0.0.1:8080/> (QEMU forwards host port `QEMU_HTTP_PORT` to the guest's port 80).
- USB device: the runtime exports the gadget over USB/IP on port `QEMU_USBIP_PORT` (3240). `make debug` attaches it automatically once it appears, and reattaches it whenever the VM drops the link (runtime restarts and audio setting changes rebind the gadget). The host then sees the AudioX USB audio interface and USB network adapter. Attaching needs the host `usbip` tool (Arch: `usbip`, Debian/Ubuntu: `usbip` or `linux-tools-generic`), the `vhci-hcd` kernel module, and `sudo`; `make debug` asks for the sudo password before booting. If you skip that, run `make debug-attach` in another terminal while the VM runs. Detach with `sudo usbip detach -p <port>` (see `usbip port`); while `make debug` runs it reattaches, so set `QEMU_USBIP_ATTACH=0` to keep it detached.
- USB network: over USB/IP the host names the NCM interface after its virtual port (for example `enu1i3`, not `usb0`), so NetworkManager profiles bound to `usb0` don't apply. The attach step brings it up with an in-memory DHCP profile (`audiox-qemu-usbip`, no default route), making the web UI reachable at `http://<CONFIG_DEVICE_IP>/`. Without NetworkManager, run a DHCP client on that interface yourself.
- USB audio does not pass audio in QEMU. The device enumerates and the host can open its streams, but UAC2 uses isochronous transfers, which the kernel's `usbip-vudc` doesn't implement (they fail with `-EXDEV`), so no samples move in either direction. Test audio on real hardware. While the host keeps a stream open, it retries isochronous transfers continuously, which costs host and VM CPU.
- The display is a virtio-gpu framebuffer at the touchscreen's 800x480. `make debug` doesn't show it; `make debug_ui` opens it in a window (`QEMU_UI_DISPLAY=gtk` by default, or `sdl`). The serial console stays in the terminal either way.

The disk image (`out/qemu/disk.img`, `QEMU_DISK_SIZE_MB`) is built without root using `mtools` and `mkfs.ext4 -d`. It persists between runs: settings and recordings on the storage partition are kept, and each `make debug` refreshes the boot files. Runtime updates installed from the web UI are not used, because QEMU always boots the freshly built initramfs. `make debug-reset-disk` deletes the disk; `make debug-clean` deletes `out/qemu/`.

Host requirements: `qemu-system-aarch64`, `mtools`, `dosfstools`, `e2fsprogs`, `util-linux` (`sfdisk`), plus `binutils` (`ar`) and `xz` to unpack the kernel package. Useful variables: `QEMU_HTTP_PORT`, `QEMU_USBIP_PORT`, `QEMU_USBIP_ATTACH=0` (don't attach), `QEMU_UI_DISPLAY`, `QEMU_MEMORY`, `QEMU_SMP`, `QEMU_CMDLINE`, `QEMU_EXTRA_ARGS`. On an aarch64 host with `/dev/kvm`, the VM uses KVM.

If gadget setup fails, include the `[INIT] [ERR] open ...` or `write ...` line preceding the fatal setup error when reporting it; it identifies the configfs attribute and kernel error. Runtime logs are stored on the storage partition as `stdout.log` and `stderr.log`; after a watchdog restart, check `stdout.log.previous` as well. `make test-system` checks the sysfs/configfs write helper's success and failure handling.

The kernel gadget survives a runtime-only watchdog restart. Startup unbinds an existing gadget and removes its audio/network configuration links before reapplying settings, then binds it normally. This causes a USB disconnect/re-enumeration rather than failing on attributes still in use. In QEMU, `make debug` reattaches the host automatically after this; when attaching from another terminal, run `make debug-attach` again. `make test-gadget-setup` checks fresh startup and restart preparation.

### Desktop audio engine bench

Build and run the host audio testbench with `make audio-bench` and `make run-audio-bench`. It compiles the shared effect and graph-block processors without the Raspberry Pi runtime, plays a 440 Hz test tone through the system's default ALSA output, and serves a live effect control page at `http://127.0.0.1:8765`. To use a WAV source, run `make run-audio-bench BENCH_INPUT=/path/to/audio.wav`; to test headlessly, use `make run-audio-bench BENCH_ALSA_DEVICE=null`. Effect types, bypass, and parameters are changed live from the page. Playback prefers 32-bit PCM for quiet-signal resolution and falls back to 16-bit when the output does not support it. `make test-audio-engine` exercises the shared block graph, PCM conversion, and effect controls.

The Cut effect passes audio when bypassed and outputs silence when enabled. Its `Channels` setting changes matching input/output port counts; since this changes routing topology, it is not MIDI-CC-mappable, but its mute state can be mapped to a MIDI note or toggled from the Web UI. Like other effects, its instance, mute state, and channel count persist across reboots.

Run `make test-soundboard-config` to check Cut persistence, missing-sound MIDI cleanup (including controller LED output), tab/space-indented configuration parsing, and touchscreen polling with and without an input device. Configuration keys and values accept leading spaces and tabs. Touchscreen polling waits for a valid device rather than repeatedly logging invalid-descriptor errors when no touchscreen is connected. This host test requires ALSA development files and uses an isolated temporary configuration directory and synthetic touch events; no touchscreen hardware is required.

The Pi runtime defaults to the direct graph-processing loop (`AUDIOX_USE_SHARED_GRAPH_PROCESSOR=0`) to avoid per-block adapter dispatch, stack graph-node setup, and shared processor code in the firmware binary. Set `AUDIOX_USE_SHARED_GRAPH_PROCESSOR=1` to compile the callback-based graph path for target-side comparison; the desktop engine uses the shared path. `processing.o` is rebuilt when the mode changes. Capture stream ownership, device attachment/remapping, and source-specific ring/SRC state live in a separate fixed-capacity ALSA capture backend. PCM I/O remains on the direct Pi path.

Bluetooth streaming logs connection events, warnings, and errors without emitting the former per-second A2DP queue/rate/packet debug statistics. The periodic PCM-overrun check remains active and still disconnects a glitching output stream.

### USB audio levels and host volume (1.10)

USB audio uses neutral input scaling; the old fixed 10x input boost has been removed. Existing saved Web/MIDI gains are not rewritten, so setups that compensated for that boost may need their gains adjusted. No limiter or automatic makeup gain is added.

The UAC2 gadget explicitly advertises master volume from -96 to 0 dB in 1 dB steps, plus mute, in both directions. The shipped kernel exposes these host settings as `PCM Capture/Playback Volume` and `Switch` ALSA controls but does not scale the samples itself. AudioX polls them every 50 ms outside the realtime thread and applies attenuation once at each USB boundary. Gadget **capture** is Windows/Linux playback into AudioX; gadget **playback** is AudioX recording into the host. These host settings remain independent of Web/MIDI gains. If the gadget ALSA device disappears, is reloaded, or its controls cannot be read, USB audio defaults to silence until valid settings are read; failures and recovery are logged. No control handles are retained between polls, so card renumbering cannot reuse an old handle. A cable disconnect alone can leave the gadget ALSA device and its kernel-maintained settings present.

Run `make test-gadget-volume` for host control, mute, reconnect, signed PCM, and capture-underrun regression checks. Before a stable release, test both directions on Windows and Linux with a single unity-gain route, effects disabled, host volume at 0/25/50/100%, and mute. At unity, a -12 dBFS test tone should stay within 0.5 dB of its input level without clipped samples. Check again after reconnect, reboot, gadget reload, and supported format changes. Windows percentages need not map linearly to amplitude. These hardware checks are still pending; host tests alone do not establish Windows compatibility.

### Kernel fault diagnostics (1.10)

At each runtime start (including a watchdog restart), AudioX saves up to the last 1 MiB of each previous runtime log as `stdout.log.previous` and `stderr.log.previous`, then starts fresh logs. An empty old log does not erase the previous nonempty evidence. Current stdout/stderr logs retain their existing unbounded behavior; the retained previous copies are bounded.

A nonblocking `/dev/kmsg` reader captures the initial kernel backlog and later messages to `kernel.log`, retaining one `kernel.log.previous` generation. Each kernel file is limited to 256 KiB; the collector reads at most 32 records per 100 ms and syncs dirty data every 10 seconds, rather than syncing each message. Its header records the boot ID and runtime/kernel identity so process restarts can be distinguished from reboots. Capture errors are reported but do not prevent audio startup. A kernel ring-buffer overrun is reported and capture continues.

If the kernel supports mounting pstore and already has a working persistent-storage backend, its existing crash records are copied non-destructively into `pstore.log` (up to 256 KiB, with one previous generation). No reserved-memory region, ramoops backend, kernel update, or panic policy is enabled by this feature. An empty/unsupported pstore does not prove panic capture works. Runtime/kernel/pstore log files are not included in configuration backups.

Download evidence before further restarts or log rotation:

```bash
curl --fail http://audiox.local/api/rootfs/kernel.log -o kernel.log
curl --fail http://audiox.local/api/rootfs/kernel.log.previous -o kernel.log.previous
curl --fail http://audiox.local/api/rootfs/stdout.log.previous -o stdout.log.previous
curl --fail http://audiox.local/api/rootfs/stderr.log.previous -o stderr.log.previous
# Only present when persistent crash records have been collected:
curl --fail http://audiox.local/api/rootfs/pstore.log -o pstore.log
```

Ordinary kernel logging can lose the final panic messages, especially during a hard hang or power loss. Without a verified persistent backend, capture the serial console to another machine: use a 3.3 V UART adapter (never RS-232/5 V), connect ground and the Pi's UART TX to the adapter RX, and do not connect its power pin. With the device off, configure the appropriate UART console (normally `console=serial0,115200`) in the boot command line, then record the adapter at 115200 baud through boot and the fault. Confirm the selected UART/pins for the board and Bluetooth overlay before wiring; existing `enable_uart=1` alone does not route kernel output to serial. Do not induce a panic on a device with unsaved state.

Run `make test-diagnostics` for retention, rotation, size limits, partial/interrupted writes, storage failures, and optional crash-record collection. The rare idle Pi 4 faults reported on 1.7/1.8 have not been reproduced or declared fixed. A trace-driven investigation and at least 24 hours of Pi 4 idle soak remain release checks.

### USB network, DHCP, DNS, and mDNS (1.10)

The device address on the USB NCM interface (`usb0`) is now `172.31.73.1/24` (`CONFIG_DEVICE_IP`). It replaces the link-local `169.254.1.2/16`, because DHCP clients do not accept leases in `169.254.0.0/16`. A static [dnsmasq](https://thekelleys.org.uk/dnsmasq/doc.html) runs on the device and serves only `usb0`:

- DHCP pool `172.31.73.10`-`172.31.73.250`, 12 h leases. The pool advertises the device as the DNS server and `lan` as the search domain.
- No router option, so the host's default route and internet access are unchanged.
- DNS answers `audiox`, `audiox.lan`, `audiox.local`, and `audiox.wklab.net` with `172.31.73.1`. Queries for other names are refused, not forwarded, so the host falls back to its other DNS servers.

Hosts often never ask the device's DNS server: VPN clients such as Tailscale, other adapters, or a router that also uses `lan` can answer first. The runtime therefore also runs a built-in mDNS responder (`src/http/mdns.cpp`). It answers `audiox.local` on `usb0` only, regardless of the host's DNS setup. It is supported out of the box by Windows 10 1709+, macOS, iOS, Android 12+, and Linux with nss-mdns/Avahi or systemd-resolved mDNS. The responder:

- Answers A queries for `audiox.local` with the current `usb0` IPv4 address, and reverse PTR queries for that address. It sends an NSEC record for other types (for example AAAA), so hosts do not wait for an IPv6 answer.
- Handles standard multicast queries, unicast-response (QU) queries, and legacy unicast queries such as `dig -p 5353 @224.0.0.251 audiox.local`.
- Announces itself when `usb0` gets its address and whenever a host is plugged in. It follows interface and address changes and pauses while `usb0` is down.
- Does not probe for name conflicts, because the USB link only connects to one host. The hostname is `AUDIOX_MDNS_HOSTNAME` (default `audiox`) in `include/defs.hpp`.

Run `make test-mdns` for packet-level tests (query types, legacy/QU modes, name compression, malformed and random packets) under AddressSanitizer/UBSan.

The PID 1 watchdog starts `/sbin/dnsmasq --keep-in-foreground --conf-file=/etc/dnsmasq.conf` in its own process group before the runtime. It restarts dnsmasq with backoff if it exits, and runtime restarts do not affect it. dnsmasq uses `bind-dynamic`, so it picks up `usb0` whenever the gadget appears or is rebound. Its log goes to the console. If the binary or config is missing, the watchdog logs it and continues without DHCP and DNS.

`make` downloads dnsmasq (`DNSMASQ_VERSION`, pinned by `DNSMASQ_SHA256`), then builds a static aarch64 binary with `scripts/build_dnsmasq.sh`. The image includes the binary at `/sbin/dnsmasq` and its licenses in `/usr/share/licenses/audiox/dnsmasq/`. The config is `root/etc/dnsmasq.conf`; the build copies it with the rest of `root/`. If you change `CONFIG_DEVICE_IP`, update the config to match; the build fails if they disagree. Run `make test-dnsmasq-config` to check consistency and syntax with the target binary (through `qemu-aarch64` when available). Run `make test-watchdog` to check service supervision.

Host notes:
- Use `http://audiox.local/`. `http://audiox/` also works on hosts that send DNS queries to the device, and `http://172.31.73.1/` always works once the host has a lease.
- Backup URL: `http://audiox.wklab.net/`. Use it if `audiox.local` does not resolve, for example on Windows with Tailscale's "Use Tailscale DNS settings" enabled, which sends every name, `.local` included, to Tailscale's resolver. `audiox.wklab.net` has a public DNS record pointing to `172.31.73.1`, and the device's own DNS also answers it, so it works even without internet when the host asks the device's DNS.
  - **Expect a browser security warning.** Because it is a public domain name served over plain HTTP, browsers mark it "Not secure", and Chrome's HTTPS-by-default mode shows a "connection is not secure" page first. Click through ("Continue to site") to open the web UI. `.local` names and the raw IP address do not get this warning.
  - The warning is about the name, not the connection. `172.31.73.1` is a private address on the USB link, so traffic goes over the USB cable to the AudioX and never touches the internet. Only the DNS lookup goes out.
  - It only reaches your AudioX while it is plugged in and has given the host a lease. Without that, the address points nowhere (or to whatever owns `172.31.73.1` on your own network, which is unlikely).
  - Some routers block public names that point to private addresses ("DNS rebinding protection"). In that case use `http://172.31.73.1/` directly.
- Hosts need a DHCP client on the AudioX interface (the default on Windows and on NetworkManager "Automatic (DHCP)" connections). Delete any old `audiox-usb0` link-local connection: `sudo nmcli connection delete audiox-usb0`.
- Browsers may treat a bare `audiox` as a search term; include `http://`.
- mDNS has been tested on the wire on Linux. Windows and macOS have not yet been tested on hardware.

### Imager publishing

The GitHub Actions build workflow runs separate Pi 4 (`make defconfig`) and Pi 5 (`make pi5_defconfig`) builds. Each uploads an SD card image, update archive, bootloader initramfs, and runtime initramfs, with artifact names suffixed by `-pi4` or `-pi5`. Pi 5 builds remain untested on hardware.

Publishing runs entirely in GitHub Actions using the repository's `GITHUB_TOKEN`:

Automatic main-branch builds run only when image/build inputs change: `src/`, `include/`, `root/`, `web/`, the Makefile, Kconfig, module lists, logo assets, or the configuration/image/firmware build scripts listed in the workflow's `push.paths`. Documentation, workflows, Imager catalogs, publishing-only scripts, and tests alone do not trigger a nightly build or release. A mixed commit containing image inputs still builds. Pull-request builds and manually requested stable releases are unchanged. Add new image input locations to this filter when extending the build. This is input-based filtering, not binary deduplication: images embed the commit ID and contain generated filesystem/archive metadata, so identical source behavior does not imply identical image checksums.

- **Nightly:** after both main-branch builds succeed, the workflow publishes a versioned `nightly-<workflow-run-number>` prerelease containing both images, checksums, and an `images.json` manifest. It keeps the **last 25 nightly builds**, newest first in Imager. Each build has its own download URLs; published versioned assets are never overwritten, including on reruns. After the updated catalog is public, older nightly releases and their tags are deleted. The rolling `nightly` prerelease still provides `audiox-latest-nightly-pi4.img.xz`, `audiox-latest-nightly-pi5.img.xz`, and `os_list.json` at fixed URLs, but its catalog points to the versioned images. The firmware version keeps the Makefile's prerelease and adds a `nightly` identifier and the commit ID (see [Firmware versioning](#firmware-versioning)). Older workflow runs cannot overwrite a newer published nightly.
- **Stable:** open **Actions > Release AudioX Stable > Run workflow**, select `main`, and enter a version such as `2.0.0` (no `v` prefix). The workflow creates `v2.0.0` at the main commit selected when the run starts, then calls the existing build workflow to build both boards, stage assets in a draft release, and automatically publish it as latest stable after all uploads succeed. Firmware versions are set from the tag; there is no further approval step. Existing tags are reused only if they point to the same commit, never moved. Stable assets are named `audiox-latest-pi4.img.xz` and `audiox-latest-pi5.img.xz`. **All stable releases** with Imager manifests appear in the Stable catalog; stable releases are never pruned or overwritten. If a job fails, use **Re-run failed jobs** to retry the same release. For an existing tag at an older commit, **Build AudioX OS Image > Run workflow** still accepts a `vX.Y.Z` tag directly. Merely pushing a tag does not publish a stable release.
- **Pull requests:** build artifacts only; never publish releases.

#### Firmware versioning

Firmware versions follow [SemVer](https://semver.org/) and come from Makefile variables: `AUDIOX_VERSION_MAJOR`, `AUDIOX_VERSION_MINOR`, `AUDIOX_VERSION_PATCH`, `AUDIOX_VERSION_PRERELEASE` (for example `beta.3`, empty for a final release), `AUDIOX_VERSION_BUILD` (build metadata after `+`), and `AUDIOX_VERSION_NIGHTLY` (`0` or `1`). Run `make print-version` to see the result.

`AUDIOX_VERSION_NIGHTLY=1` appends a `nightly` identifier to the prerelease and, unless `AUDIOX_VERSION_BUILD` is set, the 12-character git commit as build metadata:

| `AUDIOX_VERSION_PRERELEASE` | Normal build | `AUDIOX_VERSION_NIGHTLY=1` |
| --- | --- | --- |
| `beta.3` | `1.10.0-beta.3` | `1.10.0-beta.3.nightly+<commit>` |
| (empty) | `1.10.1` | `1.10.1-nightly+<commit>` |

Nightly CI builds set `AUDIOX_VERSION_NIGHTLY=1` and keep the checked-in prerelease. Stable releases clear the prerelease and build metadata and take `X.Y.Z` from the tag. A nightly built after `beta.3` sorts after `1.10.0-beta.3` and before `1.10.0-beta.4` and `1.10.0`.

`scripts/package-imager.sh` rejects versions that do not match the channel (`X.Y.Z` for stable, `X.Y.Z-[<prerelease>.]nightly+<commit>` for nightly). The Imager and download-site entry name shows the version without the nightly marker and commit, for example `AudioX 1.10.0-beta.3 Raspberry Pi 4 (nightly)`; the full version appears in the Imager description, the release notes (`Version: ...`), the download site, and the device's update check.

Point the Imager fork's repository URL at:

```text
https://raw.githubusercontent.com/wk1093/audiox/main/os_list.json
```

The checked-in [os_list.json](os_list.json) includes the Imager version/update URL and Pi 4/Pi 5 hardware selectors, using `pi4-64bit` and `pi5-64bit` tags that match the generated image entries. It offers **AudioX Stable** and **AudioX Nightly**, each loading its generated history with Pi 4 and Pi 5 choices, icons, device filters, image sizes, and SHA-256 hashes. Names include the firmware's base version, for example `AudioX 1.9.1 Raspberry Pi 4 (nightly)`; descriptions include the full firmware version, release tag/build number, and date to distinguish nightlies with the same base version. Both catalogs reference version-specific image URLs. The Stable category becomes usable after the first stable release, and Nightly after the first successful main build. No generated manifest commits or external hosting are needed. Forks must update repository-specific URLs in the checked-in manifest.

The repository must allow Actions to write release contents and update/delete nightly tags. Repository release immutability must be disabled to allow rolling nightly assets and history pruning. Direct rolling-image downloads are not atomic during replacement, but Imager uses versioned URLs and is unaffected by those replacements. Cached catalogs referring to an expired (older than the last 25) nightly must be refreshed. Failed catalog publication does not prune history; rerun the workflow to finish publication. On the first history-enabled nightly build, the existing rolling-only nightly is archived as `nightly-legacy-<release-id>` and counts toward the 25-build limit. Nightlies already overwritten before this change cannot be recovered. Stable history is rebuilt on each stable publication, including older releases with the previous `os_list.json` manifest format.

Run `bash tests/imager_package_test.sh` for image names, metadata, and checksums, and `bash tests/imager_publish_test.sh` for history, migration, pruning, and failure handling using a local mock of GitHub (requires `jq` and `xz`; publishes nothing).

### Browser-driven updates

New stable and versioned nightly releases include `audiox-update-pi4.axu` and `audiox-update-pi5.axu`, plus SHA-256 sidecars. The rolling `nightly` release also carries the latest copies. AXU archives follow the same release retention policy as their images; previously published releases are not backfilled.

On the Web UI's **System > Firmware Update** card, select Stable or Nightly and click **Check GitHub Releases**. The browser reads the Pi's `/api/system/info` response (including the hardware-detected `board` field), queries GitHub's public API, and selects only the matching board's AXU asset. Nightly lookup resolves the rolling release to a versioned nightly so its metadata does not change during download. The current version and selected release are displayed; already-installed releases cannot be installed again through the online button. Other installs, including downgrades, require explicit confirmation.

**One-time repository setup:** under **Settings > Pages > Build and deployment**, set **Source** to **GitHub Actions**. Allow the `github-pages` deployment environment to deploy from `main`. After a successful release publication, the **Deploy browser update mirror** job calls **Deploy AudioX Downloads**, downloads only the latest stable/nightly AXUs from GitHub Releases, verifies their sizes and available GitHub digests, and deploys a Pages site at `https://wklab.net/audiox/`. The device updater requests this custom-domain URL directly because the `github.io` URL redirects, and browsers block cross-origin redirects without CORS headers. The caller stable workflow grants Pages/OIDC permissions to the reusable build workflow as well.

The public site shares the device Web UI's stylesheet, colors, logos, and single-HTML navigation. **Home** introduces AudioX and its routing, effects, MIDI/soundboard, and configuration features. **Download** offers Stable/Nightly selection, separate Pi 4/Pi 5 cards, SD images for fresh installs, AXU updates, SHA-256 checksums, and release-history links. Link directly to the Download tab with `https://wklab.net/audiox/#download`; browser Back/Forward and refresh preserve the selected page. SD images link directly to GitHub Releases; AXUs download from Pages. The site includes `updates.json` and up to four latest board/channel archives, not the complete release history. Older downloads remain in GitHub Releases under the existing retention policy.

Site sources live in `site/`; `scripts/mirror-updates.sh` assembles the deployable site and catalog. Changes to the site or mirror script deploy independently without creating a nightly image build. Shared device styles/logos also redeploy the site (and still trigger normal firmware builds). Run **Deploy AudioX Downloads** manually from Actions to retry a deployment or refresh the site without rebuilding firmware.

GitHub release-storage downloads do not provide the cross-origin headers needed for JavaScript to read their bytes, so the browser uses the CORS-enabled Pages mirror for the actual AXU transfer. It requires the mirror's release tag, asset path, and size to match GitHub metadata before enabling installation. If Pages deployment has not finished, the UI reports that and leaves the direct GitHub download link available for manual file upload. A Pages failure does not undo a successful GitHub Release; rerun the failed mirror job. The mirror replaces this repository's Pages site, so use a separate hosting location if the repository already has a site you need to preserve. Forks must change both `repository` and `mirror` in `web/updates.js`, the project links in `site/index.html`, plus the Imager catalog URLs.

The **browser/computer needs internet access; the Pi does not**. The browser buffers the archive (up to 256 MiB), checks its length and AXU magic, then sends it to the existing `PUT /api/update` handler with upload progress. That handler still validates the archive's CRCs, hardware metadata, and boot configuration before installation and reboot. No GitHub token is stored in the browser or device. GitHub API rate limits, offline browsers, unavailable mirrors, and upload failures are reported in the update status. Manual `.axu` upload remains available. Older runtimes that expose only a Pi 4/5 Model B model string are supported, but need an initial manual update to obtain this new UI.

Run `node --test tests/web_updates_test.js` for browser-side selection, mirror/version checks, downloads, confirmation, and upload tests; `bash tests/imager_publish_test.sh` also exercises AXU publishing and mirror generation without contacting GitHub.

### Configuration backup and restore

Open **System > Configuration Backup** in the device Web UI:

1. Save any pending UI edits. Click **Backup Configuration** and wait for the browser to finish downloading `audiox-backup.axb` before reflashing. The download streams to disk rather than buffering the sound library in JavaScript. Errors are displayed in the download tab.
2. Flash the new image and reconnect to its Web UI. Select the `.axb` file and click **Restore Configuration**.
3. Confirm replacement, wait for upload and validation, then let the device reboot. Reload the page after it returns.

The full backup contains these persistent files under `/audiox`:

| Contents | Files |
| --- | --- |
| Audio, USB audio format, UI settings, and saved pending configuration | `config.txt`, `config.staging.txt` (when present) |
| Routing, effect nodes, and routing helper state | `routing.txt` |
| MIDI note/action/CC mappings, lighting, sampler settings, sound modes, effect state | `midi_map.txt` |
| Effect parameters and output volumes | `fx_params.txt`, `volumes.txt` |
| Bluetooth device names and pairing keys | `bluetooth/paired.txt` |
| All soundboard audio, including subdirectories and empty folders | `sfx/` |

Restore **replaces**, rather than merges, this configuration and sound library. Files missing from the backup are removed from the corresponding managed locations so defaults or stale MIDI/effect settings do not leak into the restored setup. Firmware, boot-partition files, logs, staged firmware updates, and boot-recovery state are not included or overwritten. The new image must support the saved configuration and attached hardware; moving Bluetooth pairings to a different Pi may require pairing again. Keep backups private: they contain Bluetooth keys and are not encrypted.

The device validates the archive format, full CRC32 checksum, paths, types, sizes, duplicates, and required entries before publishing a pending restore. It applies the staged files at the next boot, before configuration readers or audio/Bluetooth worker threads start. Interrupted installation resumes on the next boot; old files are kept until all replacements are installed. Invalid or incomplete uploads leave the live configuration unchanged. Do not edit configuration, upload sounds, or start a firmware update while a native backup download is in progress.

Limits: **4 GiB** per archive, **8,192** entries, at most **16** path components, and **1 MiB** per settings file. Symlinks and special files are rejected instead of following them outside the sound library. Device storage must accommodate the temporary archive and extracted contents alongside the existing setup; restore requires at least twice the upload size plus 16 MiB free. A backup download also temporarily needs enough free device storage for the archive. Transfers report progress to the main-loop watchdog.

API: `GET /api/config/backup` returns an attachment; `PUT /api/config/restore` accepts raw `.axb` bytes and returns `202 Accepted` once validated and staged, then requests a reboot. If automatic reboot fails, reboot manually. The uncompressed `.axb` v1 format uses magic `AXBACKUP`, little-endian version and entry count, path/type/size entries, and a final CRC32 over all preceding bytes. It is separate from firmware `.axu` archives.

Run `make test-config-backup` for native round-trip, streaming HTTP, corruption/path validation, and interrupted-restore tests, and `node --test tests/web_backup_test.js tests/web_updates_test.js` for browser backup/restore and firmware-updater regression tests.

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

Every update path (`/api/update`, `/api/initram`, `/api/boot/loader`, `/api/boot/kernel`, `/api/boot/config.txt`, `/api/boot/cmdline.txt`, and therefore `make dev` and `make upload_update`) is protected by an automatic rollback. State lives on the config partition under `/audiox/system/`:

1. Before a running image changes any boot file, it copies the current `config.txt`, `cmdline.txt`, `initramfs.cpio.gz`, the kernel that `config.txt` boots (for example `kernel8.img`), and `program.cpio.gz` plus its `/etc/audiox-version` into `boot-backup/`, then writes `boot-trial` (`attempts=0`). If a backup already exists it is kept, because it is the last confirmed-good set.
2. On each later boot (after any staged runtime install), the bootloader increments `attempts` and tells the PID 1 watchdog it is a trial boot.
3. During a trial boot the watchdog reboots instead of restarting the runtime when it exits, crashes, or misses its heartbeat, and appends the reason to `boot-trial.log`. Kernel panics reboot through `panic=10` on the kernel command line.
4. Once the runtime has stayed healthy for 60 seconds, it deletes `boot-trial`, `boot-trial.log`, and `boot-backup/`, so a later one-off failure cannot revert a confirmed image.
5. If a boot starts with 3 attempts already used, the bootloader writes `last-boot-failure.txt` (failed and restored versions, attempts, restored files, trial events, and the tail of `stdout.log`/`stderr.log`), copies the backup back to `/boot`, clears the trial state, and reboots.

The web UI shows a persistent banner while `last-boot-failure.txt` exists; dismissing it deletes the report.

Limitations: rollback relies on the new bootloader and kernel getting far enough to run the bootloader and to reboot. A kernel or bootloader that hangs before that cannot recover itself. `panic=10` is only written by `make export` and `make image`, so existing cards need it added to `cmdline.txt` by hand. The first update to a firmware with rollback support is not protected, because the old runtime does not create a backup.

## Notes

- Module seeds live in `depmod.txt`: use `base:` for early boot/UI-critical modules and `normal:` (or no prefix) for the rest
- Bootloader-only storage/filesystem module seeds live in `bootmod.txt`
- Both lists accept board filters for seeds that differ between boards: `base(pi5): drivers/foo` (Pi 5 only), `normal(pi4,pi5): drivers/bar`, or `normal(!pi5): drivers/baz` (all boards except Pi 5). Unfiltered seeds apply to every board. The board comes from the active config, unknown boards or prefixes fail the build, and skipped seeds are listed in `out/module-load.summary.txt`. Run `make test-resolve-modules` to test the parser
- Default Pi 4 boot graphics stay on FKMS (`vc4-fkms-v3d-pi4`; the Pi 5 defaults to `vc4-kms-v3d-pi5` because it has no FKMS); framebuffer rendering stages into an off-screen buffer before presenting to reduce visual tearing without switching the display stack
- v1.1 adds UI revamp with 4-page design (Routing, Config, Soundboard, System), per-card MIDI assignment with automatic polling, system info endpoint, improved Flexbox layout for routing page, and DELETE file API
- v1.0.2 was the previous stable release with real-time audio processing, touchscreen UI, and HTTP control

The USB network is plug-and-play: the host gets an address over DHCP and can open `http://audiox.local/`. See [USB network, DHCP, DNS, and mDNS](#usb-network-dhcp-dns-and-mdns-110). If the host has no working DHCP client on the interface, set a static address manually (the interface name varies by host):
```bash
sudo ip addr add 172.31.73.2/24 dev usb0
sudo ip link set usb0 up
```

Quick endpoint examples (`http://172.31.73.1/` also works):
```bash
# static web page
curl http://audiox.local/

# upload a new runtime image; the Pi reboots into the bootloader, installs it, then reboots again
curl --fail --show-error -X PUT --data-binary @out/program.cpio.gz http://audiox.local/api/initram

# get device info (version, kernel, uptime, memory, load)
curl http://audiox.local/api/system/info

# read/write/delete config files via HTTP API
curl http://audiox.local/api/rootfs/config.txt
curl -X PUT --data-binary @config.txt http://audiox.local/api/rootfs/config.txt
curl -X DELETE http://audiox.local/api/rootfs/sfx/myfile.wav

# trigger soundboard slot 0 from web/API
curl http://audiox.local/api/soundboard/trigger/0

# write staging audio config and reload (promotes to config.txt on success)
cat <<'EOF' | curl -X PUT --data-binary @- http://audiox.local/api/rootfs/config.staging.txt
usb_playback_channels=4
usb_capture_channels=2
usb_sample_rate=48000
usb_sample_size=2
EOF
curl -X POST http://audiox.local/api/config/reload
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
