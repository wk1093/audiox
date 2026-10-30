# Development and builds

AudioX builds a custom AArch64 initramfs system, not a package installed into a normal Raspberry Pi OS root filesystem. Run the commands below from the repository root. Start with `CONTRIBUTING.md` for contribution expectations.

Commands in this guide are source-tree developer commands, not commands to paste into an assumed SSH session on AudioX. Building `initramfs`, `image`, or `update_archive` produces host-side artifacts only. **Upload targets contact and modify the real device; `make dev` and `make upload_update` also trigger installation and reboots.** Export targets overwrite a mounted card's boot files; QEMU reset/clean targets delete VM data. Review each section before running its commands.

Pi 4 Model B is the current target. Pi 5 presets/artifacts remain **untested on hardware**. Host tests and QEMU are useful development tools, not proof of Pi 5 compatibility or successful USB audio transport.

## Repository map

| Path | Role |
| --- | --- |
| `src/`, `include/` | C++ runtime, bootloader, watchdog, and shared headers |
| `src/init/` | Boot stages, mounts, modules, gadget setup, logging/recovery |
| `src/http/` | HTTP/API, bounded logs, USB network setup, mDNS |
| `web/` | Device browser UI, staged into `/etc/www` |
| `root/` | Runtime rootfs seed files, including dnsmasq configuration |
| `scripts/` | Dependency, module, image, QEMU, and release tools |
| `Kconfig`, `.config` | Configuration definitions and local selected values |
| `depmod.txt`, `bootmod.txt` | Runtime and bootloader module seeds |
| `tests/`, `tests/Makefile` | Host regressions and desktop audio-bench targets |
| `docs/` | VitePress documentation; separate dependencies |
| `site/` | Public download site, distinct from device UI/docs |
| `out/` | Generated binaries, dependencies, firmware, rootfs trees, and images |

## Linux build prerequisites

The checked-in CI workflow installs the following on an Ubuntu-like host:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential autoconf automake libtool \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  cpio curl wget parted e2fsprogs dosfstools mtools \
  libarchive-tools libncurses-dev xz-utils jq pkg-config
```

You also need Git to obtain the repository/dependencies. Image creation uses Linux loop devices, mounting tools, and `sudo`; a working cross compiler alone does not provide those facilities. Build as your normal user; only the image script's privileged disk operations should require elevation.

Additional tools depend on the workflow:

- Host audio tests/bench: host ALSA development headers/library (Debian/Ubuntu: `libasound2-dev`).
- JavaScript regressions: Node.js with the built-in `node --test` runner.
- Documentation: Node.js/npm and the pinned `docs/package-lock.json`.
- QEMU: `qemu-system-aarch64`, `mtools`, `dosfstools`, `e2fsprogs`, `util-linux` (`sfdisk`), `binutils` (`ar`), and `xz`.
- USB/IP attachment: host `usbip`, a usable `vhci-hcd` kernel module, and sudo permission.
- Some helper/validation paths use `qemu-aarch64` for target executables on a non-AArch64 host.

Downloads require network access during dependency preparation. Do not mistake a successful host compiler setup for a complete offline build cache.

## Configure a board

Start with one board's defaults:

```bash
make pi4_defconfig
# Or, for experimental Pi 5 builds:
# make pi5_defconfig
make menuconfig
make print-config
```

`make defconfig` is an alias for Pi 4 defaults. A defconfig command **regenerates `.config`**; keep a copy of custom settings before resetting it.

The repository builds its own ncurses menu program. In menuconfig:

- Left/Right cycles allowed values.
- Enter edits a value.
- `R` resets to the selected board's defaults.
- `s` saves.

Switching boards also changes options still at the previous board's default; explicitly customized values can remain and need review. Use `make olddefconfig` to retain choices while adding new defaults. It also runs automatically when required by configuration dependencies.

### Settings worth checking

- **Board:** kernel image, DTBs, GPU firmware files, USB gadget controller.
- **Boot:** display/DSI overlays, cut-down firmware, Bluetooth UART selection, clock options, and kernel command line.
- **Firmware:** pinned Raspberry Pi firmware ref and Bluetooth firmware/checksum.
- **Network:** USB device IP. If it changes, edit `root/etc/dnsmasq.conf` to match and run `make test-dnsmasq-config`; see [Networking](/guide/networking#changing-the-subnet-when-building).
- **Audio:** engine sample rate and frames per block (defaults: 48,000 Hz and 128 frames).
- **Development:** debug shell, normally disabled.

The build-time engine sample rate is distinct from the runtime USB format saved under `/audiox/config.txt`. The Web UI preserves the latter's sample-rate field rather than exposing it as a sample-rate control. See [Config](/guide/config).

Boot configuration is generated as `out/dev-config.txt` and `out/dev-cmdline.txt`. The selected kernel keeps its firmware filename, such as `kernel_2712.img`; the generated `kernel=` selects it. Do not follow stale assumptions that every kernel is renamed to `kernel8.img`.

After switching boards, run `make fetch_deps` to restage boot files. Retain `rdinit=/init` in the single-line command line. Current defaults include `panic=10`; framebuffer console defaults are not a serial login setup.

## Build the runtime and bootloader

```bash
make fetch_deps
make show_kernel
make show_alsa
make alsa
make initramfs
```

`make` defaults to `make initramfs`. The explicit commands above make dependency selection visible:

- Firmware sync provides the matching kernel, modules, device trees, and overlays.
- ALSA is built from source into `out/alsa-sysroot`; static ALSA is the supported build strategy.
- SBC, Lua, dnsmasq, and Bluetooth firmware are prepared by their build dependencies. Lua/dnsmasq and configured firmware downloads use the checks specified in the build scripts/Makefile.
- Module dependencies are resolved and copied into the appropriate rootfs.
- Web assets from `web/` are copied into the runtime image.

Outputs:

| Output | Use |
| --- | --- |
| `out/initramfs.cpio.gz` | Small bootloader initramfs |
| `out/program.cpio.gz` | Runtime archive containing `/program` |
| `out/bootfiles/` | Board firmware/kernel/DTBs/overlays |
| `out/dev-config.txt`, `out/dev-cmdline.txt` | Generated boot configuration |
| `out/bootloader_rootfs/`, `out/program_rootfs/` | Staged root trees |
| `out/obj/` | Target objects and generated header dependencies |

The Pi firmware loads both initramfs files; the bootloader enters the runtime with chroot/exec. See [System boot stages](/guide/system#boot-stages-and-storage) for PID 1 and persistent-storage behavior.

### Module changes

Use `bootmod.txt` only for the bootloader's storage/filesystem requirements. Use `depmod.txt` for runtime seeds:

```text
base: drivers/example/early-driver
normal: drivers/example/later-driver
normal(pi5): drivers/example/pi5-only
normal(!qemu): drivers/example/pi-only
```

These are syntax examples, not real module paths to add. The resolver includes transitive dependencies; `base` loads early for boot/display-critical drivers, then `normal` loads later. The compatibility combined list remains available.

Board filters support `pi4`, `pi5`, and `qemu`, including comma-separated and negated filters. Unknown prefixes/boards fail the build. Inspect `out/module-load.summary.txt` for skipped seeds and use `make show_modules` / `make test-resolve-modules` while changing this area.

### FFmpeg for sound uploads

The converter is separate from the statically linked runtime. Runtime looks for `/audiox/ffmpeg`; full image creation stages it as `/ffmpeg` in the storage partition.

Provide your own AArch64 binary at `third_party/ffmpeg/ffmpeg` or specify it:

```bash
make FFMPEG_BIN=/absolute/path/to/your/ffmpeg image
```

When missing, `make ffmpeg` can download from `FFMPEG_URL` and verify `FFMPEG_ARCHIVE_SHA256`; upstream availability/licensing is not guaranteed by this guide. Review your binary's source/license and redistribution obligations.

For a dynamically linked converter, the runtime build stages libraries from `FFMPEG_CROSS_LIB_DIR` (default `/usr/aarch64-linux-gnu/lib`). Missing required cross-runtime libraries fail staging. Override that directory for your toolchain's sysroot rather than copying host x86 libraries.

## Make an SD image

```bash
make image
# Example larger image:
make image IMG_SIZE_MB=1024
```

The default output is `out/audiox.img`, default size 256 MiB. `scripts/build_image.sh` creates an MBR image with a FAT boot partition and an ext4 storage partition, uses a loop device, formats/mounts the image partitions, and copies boot files plus the converter. The script checks required space for FFmpeg staging.

This command creates an image file; it does **not** select and flash a physical SD card. Install it using the explicit card-identification/verification steps in [Getting started](/guide/getting-started). Successful image generation is not proof that the resulting image boots on hardware.

For an **already partitioned card**, `make export SD_MOUNT_BOOT=/actual/boot/mount` copies the boot artifacts into the mounted boot partition. It does not create or populate a new ext4 storage layout.

`make fancyexport` watches for newly inserted storage and exports to its first partition using `scripts/wait_and_export.sh`. This is not a disk-selection wizard: inspect the script and disconnect unrelated removable drives before using it. Both export paths overwrite boot files and are not substitutes for a configuration backup.

## Build and install an AXU

```bash
make update_archive
```

The default `out/audiox-update.axu` contains board/kernel/version metadata, generated boot config/command line, kernel, bootloader, and runtime. It is not an SD image and is not a configuration backup.

The v2 AXU format has `AXUPDATE` magic, entry CRC32s, and metadata naming the board/kernel. The device validates the full archive before replacement, then writes kernel/loader/command line/config and stages the runtime. Unknown noncritical entry types can be skipped after CRC checking; bit 31 marks an entry as required. Keep archive compatibility rules in `include/update_archive.hpp` and the [archive reference](./reference#firmware-archive-format) in mind when extending it.

Upload from System, or to an already-running compatible device:

```bash
make upload_update PI_HOST=audiox.local
```

`PI_HOST` defaults to the configured USB address; `PI_PORT` defaults to 80. If `/api/update` is missing, the target tells you to use one initial `make dev` on older firmware.

::: danger Uploads change real boot files
Back up [Config](/guide/config), confirm the destination board/model, and keep stable power before any upload. Automatic rollback has early-boot limitations. Neither `make dev` nor an AXU guarantees recovery from a broken kernel/bootloader; keep a card reader and a known image available.
:::

### Development upload targets

| Command | Behavior |
| --- | --- |
| `make dev PI_HOST=audiox.local` | Fetches/builds matching components, uploads kernel, bootloader, command line, boot config, then runtime; final runtime staging triggers installation/reboots |
| `make upload_bootloader` | Replaces bootloader file; does not itself reboot |
| `make upload_kernel` | Uploads configured kernel with its filename; does not itself reboot |
| `make upload_cmdline` | Replaces single-line command line; does not itself reboot |
| `make upload_ffmpeg` | Uploads converter to persistent storage |

The separate targets do not ensure that an unrelated existing kernel/module/runtime set is compatible. Runtime-only `PUT /api/initram` is appropriate only when its boot/kernel prerequisites match.

Very old firmware (1.9.0 and earlier) has stricter archive-entry compatibility; use the documented `make dev` transition rather than assuming it accepts today's optional command-line entry. See [System updates and reversion](/guide/system#firmware-updates) for the browser procedure and failure reports.

## QEMU debugging

Use the generic debug VM, not Pi emulation, for normal emulator development:

```bash
make debug
# With a framebuffer window:
make debug_ui
# Without automatic USB/IP host attachment:
make debug QEMU_USBIP_ATTACH=0
```

`make debug` recursively builds with `AUDIOX_QEMU=1` into `out/qemu/`, sharing dependency/toolchain caches in `out/`. It uses a pinned, checksum-verified Debian arm64 kernel because the Pi kernel does not supply the required virtio drivers.

Differences from hardware:

- Generic QEMU `virt` machine, virtio disk (`/dev/vda1` and `/dev/vda2`), and virtual `usbip-vudc.0` controller.
- Web UI forwarded at `http://127.0.0.1:8080/` by default.
- USB/IP forwarded on host port 3240 by default.
- Serial console in the invoking terminal; quit with **Ctrl-A X**.
- `make debug_ui` adds an 800×480 virtio-gpu framebuffer window (`QEMU_UI_DISPLAY=gtk`, or `sdl`).

Automatic USB/IP attachment needs `usbip`, `vhci-hcd`, and sudo. It reattaches after gadget reloads/runtime restarts. If skipped initially, run `make debug-attach` from another terminal.

The host NCM name reflects the virtual port, not necessarily `usb0`. With NetworkManager, the attach helper uses an in-memory DHCP profile named `audiox-qemu-usbip` with no default route; otherwise run a DHCP client for that actual interface.

**USB audio does not transport samples in this VM.** `usbip-vudc` lacks the isochronous transfer support UAC2 needs, producing `-EXDEV` even when enumeration/stream opening works. Leaving streams open can cause continuous retries and high CPU use. Validate audio on real hardware.

### Persistent debug disk

`out/qemu/disk.img` persists settings between runs; each debug build refreshes boot artifacts. The VM boots the freshly built initramfs rather than using a runtime installed via its Web UI.

```bash
make debug-reset-disk  # Deletes the VM disk and its saved settings
make debug-clean       # Deletes out/qemu/
```

Useful overrides include `QEMU_HTTP_PORT`, `QEMU_USBIP_PORT`, `QEMU_MEMORY`, `QEMU_SMP`, `QEMU_DISK_SIZE_MB`, `QEMU_CMDLINE`, `QEMU_UI_DISPLAY`, and `QEMU_EXTRA_ARGS`. On a suitable AArch64 host with `/dev/kvm`, the script can use KVM.

The older `make qemu` target attempts `raspi4b` emulation and is documented as not working adequately because of limited Pi GUI/USB/audio emulation. Do not use it as a hardware acceptance test.

## Test organization

Host tests and desktop bench rules are in `tests/Makefile`, included from the root Makefile. Run targets from the root; there is no single implied `make test` covering every category. Choose tests for the changed behavior.

| Area | Targets |
| --- | --- |
| Boot/recovery/supervision | `make test-boot-recovery test-watchdog` |
| Mount/configfs helpers and gadget setup | `make test-system test-gadget-setup` |
| Runtime logs/kernel diagnostics | `make test-system-logs test-diagnostics` |
| USB network/mDNS | `make test-mdns test-dnsmasq-config` |
| Module resolution | `make test-resolve-modules` |
| Backup archive/HTTP behavior | `make test-config-backup` |
| Shared audio engine / PCM recovery | `make test-audio-engine test-audio-pcm-recover` |
| Runtime audio / gadget host gain | `make test-audio-runtime test-gadget-volume` |
| Display/config/soundboard | `make test-display-hotplug test-soundboard-config` |
| MIDI, automation, embedded docs | `make test-midi-usb test-midi-script test-midi-docs` |

Some targets run multiple graph-processing variants, use sanitizers, need host ALSA headers, or build host Lua. `test-dnsmasq-config` checks the target binary (through `qemu-aarch64` where available). Consult the rule when a dependency is missing rather than treating all failures as firmware regressions.

Browser behavior uses Node's built-in runner, for example:

```bash
node --test tests/web_ui_test.js tests/web_logs_test.js tests/web_updates_test.js
node --test tests/web_backup_test.js tests/web_midi_test.js tests/web_lighting_test.js
node --test tests/web_routing_test.js tests/web_soundboard_order_test.js
node --test tests/build_dependencies_test.js tests/site_navigation_test.js
```

Packaging/release tests use local mocks, not real publication:

```bash
bash tests/imager_package_test.sh
bash tests/imager_publish_test.sh
```

Those packaging tests require tools such as `jq` and `xz`. Packet/parser/browser tests do not establish host OS, electrical, latency, or physical hotplug compatibility. For release validation, exercise actual power/cables, display/touch, USB networking, both audio directions and host gain/mute, MIDI/keyboard reconnects, and update/recovery on the target board.

### Desktop audio bench

```bash
make audio-bench
make run-audio-bench
# Alternate input or headless ALSA sink:
make run-audio-bench BENCH_INPUT=/path/to/audio.wav
make run-audio-bench BENCH_ALSA_DEVICE=null
```

The bench compiles shared effect/graph processors on the host, plays a 440 Hz tone by default through host ALSA, and serves controls at `http://127.0.0.1:8765`. It does not run the Pi runtime or validate the gadget.

The Pi defaults to `AUDIOX_USE_SHARED_GRAPH_PROCESSOR=0` (direct processing path). Set it to `1` for target-side comparison with the shared callback path. This is a build option, not a System UI switch.

## Documentation and release tooling

Documentation dependencies are independent from firmware dependencies:

```bash
npm ci --prefix docs
npm run docs:build --prefix docs
npm run docs:dev --prefix docs
# Or preview an existing static build:
npm run docs:preview --prefix docs
```

Keep development/preview servers bound to loopback; production publishes static output, not the development server. VitePress 1.6.4 is pinned by the docs manifest/lockfile. The current dependency audit reports one high finding for Vite and two moderate findings for esbuild/VitePress. These concern build/development tooling, not firmware or a deployed development server; no unsupported dependency overrides are applied. Review current audit output when updating the tooling.

CI builds Pi 4 and Pi 5 separately, packages compressed SD images/checksums/manifests, and publishes board-matched AXUs with SHA-256 sidecars. Stable releases are workflow-driven; versioned Nightly history retains 25 builds. `scripts/package-imager.sh`, `scripts/publish-imager.sh`, and `scripts/mirror-updates.sh` handle packaging, catalogs, and browser-update mirrors. A successful image build and a successful mirror deployment are separate events.

Forks must review repository/mirror URLs in `web/updates.js`, Imager catalogs, and site links before publishing. Do not change upstream URLs just to perform a local firmware build.

See [Releases and publishing](./publishing) for stable-release steps, nightly
retention, version strings, catalog behavior, and required repository permissions.

## Before submitting

1. Keep the change scoped and update the directly affected guide/API behavior.
2. Run the smallest relevant host/browser tests and build targets.
3. For boot/audio/hardware changes, state separately what was checked on a real device and what remains unverified.
4. Include board, host OS/toolchain, firmware/kernel version, exact commands, and whether the fault occurs after full flashing, AXU upload, or `make dev`.
5. Preserve current/previous logs and automatic-revert reports before another update or reboot.

Related operation guides: [Routing](/guide/routing), [Effects](/guide/effects), [Volume](/guide/volume), [Bluetooth](/guide/bluetooth), [Soundboard](/guide/soundboard), [MIDI](/guide/midi), [Macros](/guide/macros), [Config](/guide/config), and [Troubleshooting](/guide/troubleshooting).
