# Development and builds

AudioX builds a custom AArch64 initramfs system. Run these commands from the
repository root on a Linux host. See
[CONTRIBUTING.md](https://github.com/wk1093/audiox/blob/main/CONTRIBUTING.md)
for workflow and validation expectations, and [Developer workflows](../dev/)
for a subsystem map and common change/test paths.

Build targets such as `initramfs`, `image`, and `update_archive` create
host-side artifacts. `make dev` and `make upload_update` install firmware on a
device; export targets overwrite boot files on a mounted card; QEMU reset and
clean targets delete VM data. Check the target and destination before running
it.

Pi 4 Model B and Pi 5 are hardware-tested targets. The Pi 5 framebuffer UI remains untested with a connected display. Host tests and QEMU complement hardware testing but do not replace it.

## Repository map

| Path | Role |
| --- | --- |
| `src/`, `include/` | C++ runtime, bootloader, watchdog, and shared headers |
| `src/init/` | Boot stages, mounts, modules, gadget setup, logging/recovery |
| `src/http/` | HTTP/API, bounded logs, USB network setup, mDNS |
| `src/scripting/`, `include/scripting/` | Lua compiler/API, independent script runtime/state, storage, utilities and media-key definitions |
| `src/midi/`, `include/midi/` | MIDI controller input, lighting, and USB transport adapters used by scripts |
| `web/embedded/` | Device browser UI, staged into `/etc/www` |
| `root/` | Runtime rootfs seed files, including dnsmasq configuration |
| `tools/build/` | Dependency, module, firmware, and image tooling |
| `tools/config/`, `tools/plugins/` | Menuconfig and native plugin packaging tools |
| `tools/qemu/`, `tools/dev/`, `tools/release/` | VM, card-export, and release tooling |
| `config/Kconfig`, `.config` | Configuration definitions and local selected values |
| `config/modules/` | Runtime (`depmod.txt`) and bootloader (`bootmod.txt`) module seeds |
| `config/imager/os_list.json` | Raspberry Pi Imager entry catalog |
| `assets/branding/` | Shared logos and boot splash; docs icons link to these files |
| `tests/native/` | C++ regressions grouped by subsystem, including direct audio-runtime tests |
| `tests/web/`, `tests/build/`, `tests/release/` | Browser, build-tool, and publishing regressions |
| `tests/Makefile` | Public test targets, included by the root Makefile |
| `docs/` | VitePress documentation; separate dependencies |
| `web/public/` | Public download site, distinct from device UI/docs |
| `docs/TODO.md` | Contributor roadmap |
| `out/` | Generated binaries, dependencies, firmware, rootfs trees, and images |

Scripting has its own `ScriptContext`; MIDI is an event source and output
adapter, not the Lua runtime. The control loop owns script evaluation. Existing
script IDs, options, callback names, HTTP endpoints, and serialized entries in
the MIDI mapping file are retained for compatibility. `make test-script`,
`test-script-storage`, and `test-script-docs` cover scripting; the old
`test-midi-script*`/`test-midi-docs` targets remain aliases.

## Linux build prerequisites

The checked-in CI workflow installs the following on an Ubuntu-like host:

```bash
sudo apt-get update
sudo apt-get install -y \
  build-essential autoconf automake libtool \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  cpio curl wget parted e2fsprogs dosfstools mtools \
  libarchive-tools libncurses-dev xz-utils jq pkg-config cmake ninja-build
```

You also need Git to obtain the repository/dependencies. Image creation uses Linux loop devices, mounting tools, and `sudo`; a working cross compiler alone does not provide those facilities. Build as your normal user; only the image script's privileged disk operations should require elevation.

Additional tools depend on the workflow:

- Host audio tests: host ALSA development headers/library (Debian/Ubuntu: `libasound2-dev`).
- Native graph tests: LLVM development headers/library, `llvm-config`, and
  matching Clang (Debian/Ubuntu: `llvm-dev clang`, LLVM 15 or newer).
  Override `HOST_LLVM_CONFIG` and `HOST_CLANGXX` for version-suffixed tools.
  Graph compiler builds treat LLVM include directories as system headers so
  upstream header warnings do not fail AudioX's warning-as-error builds.
- CI reuses a versioned LLVM dependency release built by `Prepare AArch64 LLVM`.
  Trusted non-PR builds create it automatically when missing; pull requests
  have no publishing permission and build LLVM from source if it is absent.
  Authentication/network failures, corrupt archives, and incomplete releases
  fail explicitly instead of falling back. Releases are prereleases and are not
  marked latest, so they do not replace firmware releases.
  The tag includes LLVM 21.1.8, the Ubuntu 24.04/GCC 13 toolchain contract, and a
  hash of the source-build and packaging recipes. Archives include static
  libraries, public/generated headers, license inputs, and a revision manifest.
  Downloaded assets are checked against the SHA-256 sidecar in that immutable
  release and the locally expected manifest; this trusts repository release
  maintainers, not an independently pinned checksum. Never replace assets:
  change the recipe for a new tag. Local `make` still builds LLVM from source.
- Firmware DSP bitcode: **Clang 21**, matching the runtime LLVM 21 backend.
  Set `DSP_CLANGXX=/path/to/clang++-21` if it is not found through
  `llvm-config-21`. Ubuntu CI uses the signed [LLVM packages](https://apt.llvm.org/)
  for Noble and sets `DSP_CLANGXX=clang++-21`. Newer Clang bitcode is not
  interchangeable with an older LLVM runtime.
- Firmware WebUI assets and JavaScript regressions: Node.js/npm. `make web_assets`
  installs the pinned `web/embedded/package-lock.json` packages with lifecycle scripts
  disabled and compiles the device target of the web UI (scripts and styles minified
  with esbuild, plus the graph library/fonts and their licenses) into `out/web-device`;
  `rootfs` copies that build to the device. Set `WEB_MINIFY=0` to keep readable
  scripts for debugging. Once dependencies are cached, device operation requires no
  CDN or Google Fonts connection.
- Documentation: Node.js/npm and the pinned `docs/package-lock.json`.
- QEMU: `qemu-system-aarch64`, `mtools`, `dosfstools`, `e2fsprogs`, `util-linux` (`sfdisk`), `binutils` (`ar`), and `xz`.
- USB/IP attachment: host `usbip`, a usable `vhci-hcd` kernel module, and sudo permission.
- Some helper/validation paths use `qemu-aarch64` for target executables on a non-AArch64 host.

Downloads require network access during dependency preparation. Do not mistake a successful host compiler setup for a complete offline build cache.

Firmware builds also compile a pinned LLVM 21.1.8 AArch64 backend from source
in `out/graph-llvm`. The first build is substantial; allow additional time and
disk space. Its default parallelism is two jobs; set `LLVM_BUILD_JOBS` to
change it without changing the rest of the build. The statically linked
`/sbin/audiox-graph-compiler` is included automatically, along with LLVM's
license. Subsequent builds reuse the source and library cache. The host LLVM
used by tests is independent of this target dependency. Effect sources build
to LLVM bitcode under `out/dsp-bitcode` and ship in `/usr/share/audiox/dsp`;
the production executable contains effect state/control handling, not the
built-in sample-processing kernels. Each graph compilation optimizes and
recompiles these kernels on the device. There is no compiled-in DSP fallback.

## Configure a board

Start with one board's defaults:

```bash
make pi4_defconfig
# Or, for Pi 5 builds:
# make pi5_defconfig
make menuconfig
make print-config
```

`make defconfig` selects Pi 4 defaults. A defconfig command regenerates
`.config`; save custom settings before resetting it.

The repository builds its own ncurses menu program. In menuconfig:

- Left/Right cycles allowed values.
- Enter edits a value.
- `R` resets to the selected board's defaults.
- `s` saves.

Review board-specific options after changing boards. Use `make olddefconfig` to
retain selected values while adding defaults; it also runs when configuration
dependencies require it.

### Settings worth checking

- **Board:** kernel image, DTBs, GPU firmware files, USB gadget controller.
- **Boot:** display/DSI overlays, cut-down firmware, Bluetooth UART selection, clock options, and kernel command line.
- **Firmware:** pinned Raspberry Pi firmware ref and Bluetooth firmware/checksum.
- **Network:** USB device IP. If it changes, edit `root/etc/dnsmasq.conf` to match and run `make test-dnsmasq-config`; see [Networking](/guide/networking#changing-the-subnet-when-building).
- **Audio:** engine sample rate and frames per block (defaults: 48,000 Hz and 128 frames).
- **Development:** debug shell, normally disabled.

The build-time engine sample rate is distinct from the runtime USB format saved under `/audiox/config.txt`. The Web UI preserves the latter's sample-rate field rather than exposing it as a sample-rate control. See [Config](/guide/config).

Boot configuration is generated as `out/dev-config.txt` and
`out/dev-cmdline.txt`. The generated `kernel=` entry uses the selected firmware
filename (for example, `kernel_2712.img`).

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
- SBC, Lua, dnsmasq, and Bluetooth firmware are prepared by their build dependencies. Lua 5.4.9 comes from the `lua/lua` GitHub mirror at the immutable commit pinned in the Makefile; host and cross builds cache Git sources separately. dnsmasq and configured firmware downloads use the checks specified in the build scripts/Makefile.
- Module dependencies are resolved and copied into the appropriate rootfs.
- The compiled device web UI from `out/web-device` is copied into the runtime image.

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

Use `config/modules/bootmod.txt` for the bootloader's storage/filesystem requirements and Pi 5
cooling during updates/recovery. Use `config/modules/depmod.txt` for runtime seeds:

```text
base: drivers/example/early-driver
normal: drivers/example/later-driver
normal(pi5): drivers/example/pi5-only
normal(!qemu): drivers/example/pi-only
```

These are syntax examples, not module paths to add. The resolver includes
transitive dependencies; `base` loads early for boot/display-critical drivers,
then `normal` loads later.

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

The default output is `out/audiox.img`, default size 512 MiB. `tools/build/build_image.sh` creates an MBR image with a 128 MiB FAT boot partition and an ext4 storage partition, uses a loop device, formats/mounts the image partitions, and copies boot files plus the converter. The script checks boot payload space and required storage space for FFmpeg before creating the image. Increasing `IMG_SIZE_MB` enlarges storage, not the boot partition.

Older images have a smaller boot partition that may not fit LLVM-enabled firmware. Increasing the image default does not resize an already flashed card: back up persistent settings/files and reflash the new image, or resize its partitions separately. QEMU uses the same 128 MiB boot layout; an old undersized debug disk must be backed up and recreated with `make debug-reset-disk`.

This command creates an image file; it does **not** select and flash a physical SD card. Install it using the explicit card-identification/verification steps in [Getting started](/guide/getting-started). Successful image generation is not proof that the resulting image boots on hardware.

For an **already partitioned card**, `make export SD_MOUNT_BOOT=/actual/boot/mount` copies the boot artifacts into the mounted boot partition. It does not create or populate a new ext4 storage layout.

`make fancyexport` watches for newly inserted storage and exports to its first partition using `tools/dev/wait_and_export.sh`. This is not a disk-selection wizard: inspect the script and disconnect unrelated removable drives before using it. Both export paths overwrite boot files and are not substitutes for a configuration backup.

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

`PI_HOST` defaults to the configured USB address; `PI_PORT` defaults to 80.

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

## Test organization

Host test rules are in `tests/Makefile`, included from the
root Makefile. Run the focused targets for each change; `make test` runs the
native, WebUI, and release-workflow suites.
The host-test CI job also runs `make test-audio-graph-tsan` separately to check
graph publication with ThreadSanitizer. AudioContext-based host fixtures link
the real plugin-host lifecycle, even when they do not instantiate plugins.

| Area | Targets |
| --- | --- |
| Boot/recovery/supervision | `make test-boot-recovery test-watchdog` |
| Mount/configfs helpers and gadget setup | `make test-system test-gadget-setup` |
| Runtime logs/kernel diagnostics | `make test-system-logs test-diagnostics` |
| USB network/mDNS | `make test-mdns test-dnsmasq-config` |
| Classic Bluetooth discovery | `make test-bluetooth-hci` |
| Module resolution | `make test-resolve-modules` |
| Backup archive/HTTP behavior | `make test-config-backup` |
| Direct audio runtime / PCM recovery | `make test-audio-runtime test-audio-pcm-recover` |
| Runtime audio / gadget host gain | `make test-audio-runtime test-gadget-volume` |
| Display/config/soundboard | `make test-display-hotplug test-soundboard-config` |
| MIDI, automation, embedded docs | `make test-midi-usb test-script test-script-docs` |

Some targets use sanitizers, need host ALSA headers, or build host Lua. `test-dnsmasq-config` checks the target binary (through `qemu-aarch64` where available). Consult the rule when a dependency is missing rather than treating all failures as firmware regressions.

### Interpreting test output

Native regressions deliberately exercise failures such as short writes, full
queues, invalid configuration versions, missing devices, and corrupted telemetry
storage. These cases print the runtime's normal `[WARN]` and `[ERR]` diagnostics
even when their assertions pass. Read the test's final result and command exit
status rather than treating every runtime warning as a failed test; do not
suppress stderr, which also carries genuine failures and sanitizer reports.

Compiler warnings, assertion failures, sanitizer reports, and nonzero exits
need investigation. Lua-based host tests also fetch the commit-pinned
Lua source from GitHub when its local cache is missing. A Git connection failure during that
step is a dependency-download failure, not an audio or scripting test failure.

Browser behavior uses Node's built-in runner, for example:

```bash
node --test tests/web/web_ui_test.js tests/web/web_logs_test.js tests/web/web_updates_test.js
node --test tests/web/web_backup_test.js tests/web/web_midi_test.js tests/web/web_lighting_test.js
node --test tests/web/web_routing_test.js tests/web/web_soundboard_order_test.js
bash tests/build/build_dependencies_test.sh
node --test tests/web/site_navigation_test.js
node --test tests/web/web_bluetooth_test.js
```

Packaging/release tests use local mocks, not real publication:

```bash
bash tests/release/imager_package_test.sh
bash tests/release/imager_publish_test.sh
```

Those packaging tests require tools such as `jq` and `xz`. Packet/parser/browser tests do not establish host OS, electrical, latency, or physical hotplug compatibility. For release validation, exercise actual power/cables, display/touch, USB networking, both audio directions and host gain/mute, MIDI/keyboard reconnects, and update/recovery on the target board.

### Audio runtime tests

```bash
make test-audio-runtime test-gadget-volume test-audio-pcm-recover
```

Audio tests compile the same direct processing code used by the Pi, with
simulated device I/O. They cover route mixing, effect controls, channel isolation,
soundboard playback, Bluetooth buffers, host gain/mute, and five-second DSP idle
sleep, including silent sleep/wake transitions. They do not play through host ALSA or validate
physical gadget timing; use the target board for listening and latency checks.

## Documentation and release tooling

Documentation dependencies are independent from firmware dependencies:

```bash
npm ci --prefix docs
npm run docs:build --prefix docs
npm run docs:dev --prefix docs
# Or preview an existing static build:
npm run docs:preview --prefix docs
```

Keep development and preview servers bound to loopback. Production publishes
static output, not the development server. VitePress is pinned by the docs
manifest and lockfile.

### Web UI build targets and the Live Demo

`web/embedded/build.js` compiles one set of pages, scripts and styles into two
targets:

- `device` talks to the firmware through `api.js`. Every UI request goes through
  `apiFetch()` (or `AudioxApi.upload`/`AudioxApi.download`), never `fetch('/api/…')`
  directly.
- `demo` replaces `api.js` with `demo/mock-api.js`, a stateful in-browser
  simulation seeded by `demo/mock-data.js`, and rewrites root-relative URLs so the
  UI works below `/audiox/demo/` on the public site.

```bash
node web/embedded/build.js --target device --out out/web-device [--no-minify]
node web/embedded/build.js --target demo --out /tmp/audiox-demo
```

When you add or change an API endpoint, update the matching route in
`demo/mock-api.js` so the Live Demo keeps working.

`make site` stages the complete public website the same way the Pages workflow
does (download page, VitePress docs, and Live Demo) into `out/site/audiox`,
without the release-archive mirror. `make site-serve` builds it and serves it
with `python3 -m http.server` at `http://127.0.0.1:8000/audiox/`
(`SITE_PORT` changes the port).

Release tooling builds Pi 4 and Pi 5 images, checksums, manifests, and
board-matched AXU archives. `tools/release/package-imager.sh`,
`tools/release/publish-imager.sh`, and `tools/release/mirror-updates.sh` handle packaging,
catalogs, and browser-update mirrors; `tools/release/stage-site.sh` stages the
public site, docs, and Live Demo. Firmware publication and mirror
deployment are separate workflows.

Forks must review repository/mirror URLs in `web/embedded/updates.js`, Imager catalogs, and site links before publishing. Do not change upstream URLs just to perform a local firmware build.

See [Releases and publishing](./publishing) for stable-release steps, nightly
retention, version strings, catalog behavior, and required repository permissions.

## Before submitting

1. Keep the change scoped and update the directly affected guide/API behavior.
2. Run the smallest relevant host/browser tests and build targets.
3. For boot/audio/hardware changes, state separately what was checked on a real device and what remains unverified.
4. Include board, host OS/toolchain, firmware/kernel version, exact commands,
   and whether the fault occurs after flashing, AXU upload, or `make dev`.
5. Save relevant logs and automatic-revert reports before another update or reboot.

Related operation guides: [Routing](/guide/routing), [Effects](/guide/effects), [Volume](/guide/volume), [Bluetooth](/guide/bluetooth), [Soundboard](/guide/soundboard), [MIDI](/guide/midi), [Scripts](/guide/macros), [Config](/guide/config), and [Troubleshooting](/guide/troubleshooting).
