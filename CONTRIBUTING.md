# Contributing to AudioX

Contributions to AudioX are welcome, especially focused fixes, reliability work,
tests, hardware validation, and documentation. For a substantial feature or
design change, open an issue before implementation.

## Development environment

Builds target AArch64 and are run from the repository root on Linux. On Ubuntu,
install the base toolchain and image utilities with:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential autoconf automake libtool \
  gcc-aarch64-linux-gnu g++-aarch64-linux-gnu \
  cpio curl wget parted e2fsprogs dosfstools mtools \
  libarchive-tools libncurses-dev xz-utils jq pkg-config
```

Additional dependencies depend on the target. Host audio tests require ALSA
development files; firmware WebUI assets require Node.js/npm; browser tests
require Node.js; docs require Node.js/npm;
some target checks use `qemu-aarch64`. The [development guide](docs/guide/development.md)
lists commands, outputs, and optional tooling.

Common commands:

```sh
make show_kernel
make show_alsa
make alsa
make initramfs
make image
```

`make` defaults to `make initramfs`. The outputs
`out/initramfs.cpio.gz` and `out/program.cpio.gz` are host-side artifacts.
`make image` creates an image file; it does not flash a card.

::: danger Device and card writes
`make dev` and `make upload_update` install firmware on a real device.
`make export` and `make fancyexport` overwrite boot files on a mounted card.
Verify the destination, keep stable power, and retain a backup and recovery
image. Kernel or bootloader changes can prevent startup; automatic update
reversion does not cover every failure.
:::

## Project layout

- `src/`, `include/`: C++ runtime, bootloader, and shared headers.
- `tools/`: host tooling grouped into `build/`, `config/`, `dev/`, `plugins/`,
  `qemu/`, and `release/`. Scripts and compiled host tools live together by purpose.
- `config/`: `Kconfig` definitions, module seeds in `modules/`, and the
  Raspberry Pi Imager entry catalog in `imager/`. Local selections stay in `.config`.
- `assets/branding/`: shared logos and the boot splash. Documentation icons
  link here instead of maintaining duplicate artwork.
- `web/embedded/`: device control interface, staged into `/etc/www`.
- `web/public/`: public download site. Documentation remains in `docs/`.
- `root/`: runtime root-filesystem seed files.
- `tests/`: `native/` regressions grouped by subsystem, plus `web/`, `build/`,
  and `release/` suites. `tests/Makefile` keeps the public Make targets together.
- `docs/`: VitePress documentation.
- `out/`: generated dependencies, images, and build outputs.

Run Make targets from the repository root; the layout does not change target
names, output locations, device paths, or published site URLs. See the
[development guide](docs/guide/development.md) for individual commands and
[TODO.md](docs/TODO.md) for the roadmap. Keep new files with their subsystem
rather than adding root-level helpers.

## Contribution workflow

Use a short-lived branch from an up-to-date `main`. For example:

```sh
git switch main
git pull --ff-only origin main
git switch -c fix/describe-the-change
```

Use a descriptive branch name such as `feat/...`, `fix/...`, `docs/...`, or
`test/...`. Contributors without write access should push to a fork and open a
pull request against `main`.

Keep pull requests scoped and explain the problem, intended behavior, risk, and
validation. Update documentation when behavior changes. Open a draft PR early
when design feedback would help. Before publishing, check commits and files for
private information; anything pushed to a public remote should be treated as
public.

Prefer a squash merge with a concise title and one release-note entry per
user-facing change:

```text
Improve volume controls

[feat] Extend the volume range
[fix] Correct the unity-gain display
```

Use `[feat]` for user-facing additions and `[fix]` for corrections. Describe
documentation- or test-only changes plainly.

## Code and runtime guidelines

- Keep changes focused and follow the style of nearby code.
- Avoid new runtime dependencies unless they provide clear value.
- Prefer deterministic behavior, explicit errors, and safe fallbacks.
- Keep logs brief and actionable. Excessive logging can fill or wear the SD card.
- Preserve real-time constraints in audio processing. Avoid locks, allocation,
  or retry loops on the audio thread.
- Do not add a second consumer to the routing snapshot mailbox; the audio thread
  is its sole consumer. Publishers are serialized on the control side, while UI
  readers use a separate mutex-protected copy.
- Keep routing helper expansion on the control side. The runtime accepts at
  most 128 compiled paths; report skipped paths rather than dropping them
  silently.

## Validation

Run the smallest relevant test set while iterating. Before a release, run
`make test` and build the affected firmware target.

```sh
make test-web
make test-native
make test-build-dependencies
make test-audio-graph-tsan
```

`make test` runs native C++/Bash, Node.js, and release-workflow regression
suites. Host dependencies include a C++17 compiler, `pkg-config`,
ALSA/ncurses/SBC development headers, Node.js 22+, Python 3, Bash,
and `jq`; Make builds the host Lua library.
`make test-build-dependencies` runs Bash-only Makefile and incremental header
dependency checks. `make test-audio-graph-tsan` checks graph publication with
ThreadSanitizer. `make test-dnsmasq-config` additionally needs the target
dnsmasq build and `qemu-aarch64` (or host dnsmasq).

Host tests and QEMU do not establish physical-device behavior. Changes involving
boot, power, audio, USB, MIDI, or displays need corresponding hardware checks on
the target board.

## Pull requests and issue reports

Include in a pull request:

- The behavior changed and the reason.
- Relevant tests/builds and their results.
- Additional risk and hardware-validation details for boot, updates,
  partitioning, or audio-thread changes.

For an issue, include the Pi model, AudioX/kernel versions, host OS and
toolchain where relevant, connected hardware, exact steps and commands, full
error output, and relevant log excerpts. Do not attach a full configuration
backup publicly: it can contain Bluetooth pairing keys, private audio clips,
and script text.
