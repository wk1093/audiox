# Developer workflows

This section is for contributors changing AudioX itself. It complements the
[build and development guide](../guide/development), which covers toolchain
setup, board configuration, firmware builds, QEMU, and device installation.
For contribution scope, coding expectations, and pull requests, see
[CONTRIBUTING.md](https://github.com/wk1093/audiox/blob/main/CONTRIBUTING.md).

## Start with the relevant workflow

| If you are changing... | Start here |
| --- | --- |
| C++ runtime, boot, or build system | [Build and development guide](../guide/development) |
| Device WebUI or its API | [WebUI workflow](#device-webui-and-api) |
| Native audio plugins | [Native plugin development](../plugins) |
| Scenes and their persisted format | [Scenes](../scenes) |
| Lua scripting and its API | [Lua documentation](../lua/) |
| Telemetry collector or report schema | [Telemetry collector development](./telemetry) |
| Release publication and versioning | [Releases and publishing](../guide/publishing) |
| User-visible behavior and operation | [Documentation home](../) |

## System map

AudioX is a custom C++ AArch64 initramfs system, not an application installed
on Raspberry Pi OS. The main implementation areas are:

| Path | Responsibility |
| --- | --- |
| `src/init/bootloader.cpp` | First-stage bootloader and recovery/update flow |
| `src/init/init.cpp` | Main runtime startup and device-service coordination |
| `src/init/watchdog.cpp` | PID 1 watchdog supervising the runtime |
| `src/audio/`, `include/audio/` | Audio graph, PCM I/O, processing, effects, and audio health |
| `src/http/`, `include/http/` | Device HTTP server and API handlers |
| `src/config/`, `include/config/` | Persistent configuration and scene handling |
| `src/midi/`, `include/midi/` | MIDI input, mappings, lighting, and transport |
| `src/scripting/`, `include/scripting/` | Lua runtime, script API, and script storage |
| `src/plugin_worker/`, `src/audio/` | Isolated native plugin worker and host integration |
| `src/framebuffer/`, `src/touch/` | Local display and touch input |
| `web/embedded/` | Device WebUI and public Live Demo sources |
| `root/`, `config/` | Root filesystem seeds, Kconfig, board defaults, and module lists |
| `tools/` | Build, configuration, QEMU, development, plugin, telemetry, and release tools |
| `tests/` | Native, WebUI, build, and release regressions |
| `docs/` | VitePress user and developer documentation |

The root [Makefile](https://github.com/wk1093/audiox/blob/main/Makefile) is the
entry point for firmware builds and test targets. Generated files and build
dependencies belong under `out/`; do not edit generated output as a source
change.

## A reliable change workflow

1. Reproduce the behavior and find its source, API, persisted data, and existing
   tests before editing. Search nearby tests and subsystem documentation for
   established patterns.
2. Keep the change focused. Preserve existing file formats and API behavior
   unless changing them is part of the task; update compatibility and backup
   handling when persistent data changes.
3. Add or update a regression test with the behavior change. Prefer the
   narrowest target while iterating.
4. Build the affected firmware or WebUI target. A host test is not a substitute
   for a target build.
5. Run the broader checks appropriate to the change, then record hardware
   validation separately from host/QEMU results.
6. Update the user guide, API reference, plugin/script guide, or this developer
   section when the changed behavior affects it.

The shared contribution guidelines call out audio-thread constraints, routing
publication ownership, and expected pull request contents. In particular,
audio processing must not introduce blocking I/O, locks, allocation, or retry
loops on the realtime callback. See
[CONTRIBUTING.md](https://github.com/wk1093/audiox/blob/main/CONTRIBUTING.md)
before changing graph publication or realtime processing.

## Device WebUI and API

The source in `web/embedded/` builds two variants from the same UI:

- The **device** variant uses `api.js` to communicate with the firmware.
- The **demo** variant substitutes `demo/mock-api.js` and simulated state so
  the public Live Demo can run without a device.

When adding an API call, route it through the shared API gateway (`apiFetch`,
`AudioxApi.upload`, or `AudioxApi.download`) rather than calling `fetch` from a
feature module. Implement the corresponding behavior in `demo/mock-api.js`
when the UI needs to work in the Live Demo. Keep request/response behavior and
error states consistent between the two variants.

Build the device assets with:

```sh
make web_assets
```

For readable local output, the build script also supports an unminified build:

```sh
node web/embedded/build.js --target device --out out/web-device --no-minify
```

Run focused browser regressions with Node's built-in test runner:

```sh
node --test tests/web/web_routing_test.js
node --test tests/web/web_scenes_test.js
```

Choose the test matching the affected UI module; `make test-web` runs all
WebUI tests and telemetry Worker tests. `make site` stages the public download
site, documentation, and Live Demo locally; `make site-serve` serves that
staged site on loopback. More details about both UI build targets are in the
[build and development guide](../guide/development#web-ui-build-targets-and-the-live-demo).

## Native runtime and persisted behavior

For audio changes, start with the implementation under `src/audio/` and its
headers/tests. The host audio-runtime tests exercise the same direct processing
code with simulated device I/O; they do not prove physical USB timing, audio
quality, or latency. Use focused tests such as:

```sh
make test-audio-runtime test-audio-graph test-audio-pcm-recover
make test-audio-graph-tsan
```

For HTTP or stored configuration changes, trace the handler through the
relevant subsystem and test both success and failure behavior. Configuration
files and backup/archive formats are user data: validate before replacing
files, keep writes recoverable, and document compatibility. Existing subsystem
references include [Scenes](../scenes), [persistent files and HTTP API](../guide/reference),
and [configuration and backups](../guide/config).

Native plugins execute in an isolated worker and have their own ABI, package
format, sandbox, and lifecycle rules. Read [Native plugin development](../plugins)
before changing that boundary.

### Native graph compilation prototype

The firmware automatically requests native compilation after rebuilding a
graph. A non-realtime compiler thread launches `/sbin/audiox-graph-compiler`,
which links the shipped built-in DSP bitcode, optimizes it at O3 for the
device's CPU, specializes its frame loops to the configured block size, and
generates a single native graph containing routing, mixing, bypass/silence
handling, and direct built-in DSP calls. Route endpoints and node indices are
compile-time constants; the graph helpers are inlined into its entry point.
This recompiles all eleven built-ins, not just scheduling callbacks. LLVM itself stays outside the main
runtime. There is no build flag and no user-authored compiled effect language.

Code is adopted at a block boundary; stale generations never run against a new
graph. The compiler relocates code and constants for an address reserved by the
runtime, using explicitly supplied math/memory function addresses. It never
executes the generated DSP. The compiler thread owns mapping and retirement;
executable mappings are read/execute, not writable. Mutable generated globals
are rejected: effect state and the pitch FFT plan live in the main runtime and
survive code replacement.

**There is no compiled-in DSP fallback.** Until compilation for the exact
generation succeeds, enabled built-ins requiring sample processing produce
silence. Direct routes, bypass, Cut, and isolated native plugins retain their
existing behavior. Failed compilation or a ten-second timeout is explicitly
logged; a subsequent graph rebuild can retry. Rapid route changes can coalesce.
Expect temporary silence on routed effects after startup and routing saves.

Parameter changes do not recompile the graph. Before processing, the runtime
prepares a persistent block view with current gains/parameters and state pointers.
It then calls the native graph once. The generated
graph routes and mixes buffers and calls DSP implementations directly, without
per-route or built-in-effect callbacks into main. Effect-type selection remains
inside native code so live type changes retain their behavior. The only processing
callback is the intentional boundary to the isolated plugin worker.

Tail/release checks execute inside the native graph only when a channel is silent,
using shared helpers also used by the reference path.
Device I/O, state reset/control preparation, whole-engine sleep decisions,
and meter publication remain in the runtime. The per-thread DSP table is retained
for host reference/failure-path tests, not used to dispatch built-ins in a ready
production graph. This enables joint optimization but does not guarantee fusion
of entire effect chains into one sample loop. Conservative floating-point behavior is retained (no blanket
fast-math). Runtime slot bookkeeping and DSP bitcode disable floating-point contraction so
silent-release fixed-point checks agree with generated DSP even on FMA-capable
CPUs. Compilation can contend for CPU/memory bandwidth, and no reduction
in X-runs is established without measurements on a Pi.

Logs distinguish `LLVM compiled DSP generation ...` (size and compile time)
from `audio adopted native DSP generation ...` (actual audio-thread adoption).
Adoption logging happens on the compiler thread, not in the audio callback.

Run `make test-graph-native test-audio-runtime` for generated-code lifecycle
and all-kernel output/state-continuity comparisons, and
`make test-graph-native-tsan` for compiler mailbox/code-retirement race checks.
`test-audio-runtime` runs both reference and direct-native-graph variants covering
stereo mixing, effect tails, idle behavior, plugins, and missing-code silence.
The native graph tests also compare a fan-in/gain/distortion chain with reference
audio while forbidding routing/DSP callbacks.
Reference kernels exist only in host test executables. Build prerequisites and the LLVM source cache are
described in the [build guide](../guide/development#linux-build-prerequisites).

## Tests and continuous integration

Runtime objects depend on a content-tracked version stamp. Changing the Makefile
version or command-line `AUDIOX_VERSION_*` overrides rebuilds the runtime so
the API/UI version matches the packaged version; unchanged versions leave
incremental builds intact. Check the selected version with `make print-version`.

Test targets live in `tests/Makefile`, included by the root Makefile:

```sh
# Full native, browser, and release-workflow regression suites
make test

# Graph publication race check (also run separately in CI)
make test-audio-graph-tsan

# Build dependency / incremental header dependency regressions
make test-build-dependencies

# Documentation source links and page behavior
node --test tests/web/site_navigation_test.js tests/web/docs_site_test.js
```

`make test` includes native C++ and helper-script tests, WebUI tests, and local
release-workflow tests. Release tests use mocks and do not publish anything.
Individual test targets are listed in `tests/Makefile`; consult their recipes
when a target needs extra tools or has hardware limitations.

The `Build AudioX OS Image` workflow runs `make test`, the graph ThreadSanitizer
test, and Pi 4/Pi 5 builds for relevant source changes and pull requests. The
site workflow builds and deploys the documentation, Live Demo, and public
website when its watched site/documentation files change. Workflow definitions
are in `.github/workflows/`; local success does not replace checking the
corresponding CI jobs.

## Documentation changes

Documentation lives in `docs/` and uses VitePress. Dependencies are pinned
separately from the device WebUI:

```sh
npm ci --prefix docs
npm run docs:dev --prefix docs
npm run docs:build --prefix docs
```

The dev server is configured to bind to loopback. Add new pages to the
navigation in `docs/.vitepress/config.mts`, and check links with
`tests/web/site_navigation_test.js` and `tests/web/docs_site_test.js`. Keep
device-operation instructions in `docs/guide/`, Lua references in `docs/lua/`,
and contributor/deployment procedures in `docs/dev/`. The public telemetry
operations guide is separate from the user-facing
[optional telemetry guide](../guide/telemetry).

## Hardware validation

Host tests and QEMU are useful for deterministic behavior, API/UI development,
and many boot flows, but they do not establish Raspberry Pi behavior. Changes
to audio, USB, MIDI, display/touch, power, fan control, or boot/update paths
need suitable checks on the target hardware. Record the board, connected
devices, firmware/kernel version, exact steps, and what remains untested.

Treat installation targets as destructive operations. `make dev` and
`make upload_update` change a real device; export targets overwrite boot files
on a mounted card. Confirm the destination, keep stable power, and have a
known-good image and configuration backup. The
[build and development guide](../guide/development) describes these targets
and their recovery limitations.
