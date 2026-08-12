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
make dev # build and upload runtime (only works if you have a running AudioX device already setup)
```

WARNING: `make dev` overwrites the runtime on the device, it doesn't have the capacity (yet) to update the bootloader or kernel, so if I update the firmware to use a newer kernel, your device will not work with `make dev` because the kernel modules in the program will be newer than the kernel on the device. For now, this requires you to reflash (or use `make fancyexport`). This can also happen if you break the runtime image (kernel panic, http server crash, failed init, etc). THIS IS NOT PERMANENT, I plan on adding recovery and update safety features in the future, but for now, be careful with `make dev`.

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
2. Run the relevant target for your change (for example make initramfs or make image).
3. Update docs when behavior changes.
4. Keep commits readable and scoped.

PR description should include:

- What changed
- Why it changed
- Risk level
- How you tested it

If your change affects boot/update flow, partitioning, or audio thread behavior, include extra detail and test notes.

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