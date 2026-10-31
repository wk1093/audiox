# Contributing to AudioX

Thanks for checking out AudioX and for considering a contribution.

This project is currently maintained by one person and has grown quickly. Help is very welcome, especially on bug fixes, reliability, tests, docs, and small focused features.

## First things first

- Be respectful and constructive.
- Prefer small pull requests over large rewrites.
- Keep behavior changes explicit and documented.
- If you plan a big change, open an issue first to align on direction.

## Ways to help

Good contribution areas right now:

- Check the TODO.md for the main things I am working on, maybe you can help with one of those. Try to focus on things that are in the same version milestone, I don't want to have work on future versions get in the way of current work (If you have a suggestion for a re-ordering of the TODO.md, or new things that should be added in a certain release, feel free to open an issue or PR, I am happy to discuss and re-prioritize things).
- I would love help with testing and bug reports. If you find a bug, please open an issue with as much detail as possible.
- If you notice any issues in my code in a recent commit, please open an issue or PR. I am happy to review and merge small fixes/fix them myself.
- Boot and update safety
- Audio stability and latency improvements
- HTTP API hardening and error handling
- UI and UX polish (framebuffer and web, especially web since it had heavy AI assistance)
- Documentation updates and cleanup

## Development setup

Prereqs on Ubuntu-like host:

- build-essential
- gcc-aarch64-linux-gnu
- g++-aarch64-linux-gnu
- cpio
- curl
- parted
- dosfstools
- e2fsprogs

Common commands:

```bash
make show_kernel # show kernel build config
make show_alsa # show ALSA build config
make alsa # build ALSA from source
make initramfs # build bootloader and program initramfs
make image # build full SD card image
make dev # build and upload matching kernel, bootloader, and runtime (requires a running AudioX device)
```

WARNING: `make dev` replaces the device kernel and bootloader before staging the runtime update, so use it only with a running AudioX device. It does not provide recovery if an update breaks boot; you may need to reflash the SD card (via `make fancyexport` or `make image`). Be careful when testing kernel and bootloader changes.

## Project layout

- src/: runtime and bootloader C++ implementation
- include/: shared headers
- scripts/: build and image tooling
- web/: static web UI
- root/: runtime rootfs seed files

Two-stage boot artifacts:

- out/initramfs.cpio.gz (bootloader)
- out/program.cpio.gz (runtime)

## Coding guidelines

- Use focused changes. Avoid unrelated refactors in the same PR.
- Match existing code style in touched files. (I will probably add clang-format in the future)
- Do not add new runtime dependencies unless necessary (you will probably need to discuss this with me, I am happy to add new dependencies if they are useful, but I want to avoid adding unnecessary bloat. I also prefer lightweight solutions, and enjoy the learning process of writing things from scratch).
- Keep logs useful and brief. If the logs are too verbose, and the device is left on for a long time, the SD card can wear out quickly, and/or fill up the SD card and cause problems. I want to avoid this, so please be careful with logging.
- Prefer deterministic behavior, explicit error handling, and safe fallbacks.

## Pull request guidelines

Before opening a PR:

1. Build locally and confirm your change compiles.
2. Run the relevant tests, then `make test` before release. This runs the native
   C++/Bash, Node.js, and release-workflow regression suites. Host dependencies
   include a C++17 compiler, pkg-config, ALSA/libsndfile/ncurses/SBC development
   headers, Node.js 22+, Python 3, Bash, and jq; Make builds its host Lua library.
   `make test-native` and `make test-web` run individual suites.
   `make test-build-dependencies` runs the Bash-only Makefile and incremental
   header dependency checks without Node.js.
   `make test-audio-graph-tsan` checks graph publication with ThreadSanitizer.
   The separate `make test-dnsmasq-config` target additionally needs the target
   dnsmasq build and qemu-aarch64 (or host dnsmasq). Build the relevant firmware
   target when changing runtime code.
3. Update docs when behavior changes.
4. Keep commits readable and scoped.

PR description should include:

- What changed
- Why it changed
- Risk level
- How you tested it

If your change affects boot/update flow, partitioning, or audio thread behavior, include extra detail and test notes.

Routing snapshots use a fixed triple-buffer ownership handoff: publishers are
serialized on the control side, with exactly one audio-thread consumer. The
consumer exchanges an atomic slot index once per update, without taking a lock,
allocating, or retrying. Do not add a second consumer to this mailbox. UI readers
use a separate mutex-protected copy. ALSA rescans advance the topology generation
only when devices actually change; the control loop then republishes the graph
so saved routes resume on hotplug without periodically reopening unchanged PCMs.
Helper expansion also runs on the control side, before publication. The runtime
receives at most 128 compiled paths; skipped helper paths are counted and
reported rather than silently discarded on the audio thread.

## Reporting issues

Please include (when applicable):

- Host OS and toolchain details
- Raspberry Pi model
- Exact command run
- Full error output
- Steps to reproduce

For runtime regressions, include whether the issue appears in make dev uploads, full image flashes, or both.

## Scope and roadmap

AudioX is still pre-2.0 and actively evolving. Stability and maintainability improvements are especially valuable.

Thanks again for helping move the project forward.