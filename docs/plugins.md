# Native effect plugins

An `.axm` is an AudioX Module package containing a native mono effect, metadata,
and optional HTML/CSS/JavaScript controls. Native code runs in an isolated worker,
never in the HTTP server or audio parent process. Installing a package only parses
data and checks ELF headers; it does not load or execute its module.

## Install and use

On **Effects**, choose an `.axm` in **Plugins** and select **Install plugin**.
The Routing **Add → plugin** menu then offers its stereo effect. Unnamed instances
use the package's declared name and instance number; custom names override it.
Parameters use the existing
sliders, MIDI mappings, and Lua effect API, with stable `fx_N` instance IDs.
WebUI numeric parameter updates preserve native float precision; values are not
quantized to four decimal places. Custom controls are optional and open only when
explicitly requested.

Use **Reload** to restart a faulted plugin's workers. Crashes, forbidden syscalls,
and repeated missed deadlines produce one device log and delayed dry bypass.
Faulted workers are terminated on the control thread, without blocking audio.
Remove every live and saved instance before **Remove**. Replacing an existing
package requires removal first; changing already registered metadata also
requires a runtime restart. There are 16 installed packages and 16 distinct
registered plugin IDs per runtime lifetime; removed IDs remain reserved until
restart so saved identities cannot accidentally become another effect.

Missing packages preserve saved effect IDs/type names and bypass without native
processing. Reinstall and reload to restore their saved parameter values.
Packages live under `/audiox/plugins/`; normal configuration backups currently
exclude plugin binaries/assets. Keep the original `.axm` files separately.
This implementation is validated on Linux hosts and cross-built for AArch64;
device kernel sandbox support and performance still need physical testing.

Management HTTP endpoints:

| Method | Endpoint | Body/result |
| --- | --- | --- |
| GET | `/api/plugins` | Installed IDs, metadata, parameter ranges, UI availability |
| PUT | `/api/plugins/install` | Binary `.axm`, nonempty and at most 12 MiB |
| POST | `/api/plugins/remove` | `id=plugin_gain`, rejects references from live/saved effects |
| POST | `/api/plugins/reload` | `id=plugin_gain`, prepares replacement workers |
| GET | `/api/plugins/plugin_gain/assets/index.html` | Validated flat assets with sandbox CSP |

Installation is a trusted-local administrative operation, not a signed-package
marketplace. Do not expose the existing unauthenticated HTTP interface publicly.

## Build an example

From the repository root (a C++17 compiler and Linux headers are required):

```sh
mkdir -p build/plugins
c++ -std=c++17 -O2 -Iinclude tools/axm-pack.cpp src/audio/plugin_package.cpp \
  -o build/plugins/axm-pack
cc -std=c11 -O2 -fPIC -shared -fvisibility=hidden -Iinclude \
  examples/plugins/gain/gain.c -o build/plugins/gain.so
build/plugins/axm-pack examples/plugins/gain/axm.txt build/plugins/gain.so \
  build/plugins/gain.axm examples/plugins/gain/index.html \
  examples/plugins/gain/controls.js examples/plugins/gain/style.css
cc -std=c11 -O2 -fPIC -shared -fvisibility=hidden -Iinclude \
  examples/plugins/softclip/softclip.c -o build/plugins/softclip.so
build/plugins/axm-pack examples/plugins/softclip/axm.txt \
  build/plugins/softclip.so build/plugins/softclip.axm
```

The packer refuses to overwrite its output. Delete an old `.axm` explicitly before
repacking. For an AArch64 device, use your AArch64 cross-compiler in place of `cc`.
The host packer accepts both x86_64 and AArch64 ELF64 modules; installation requires
the device's native architecture. No dependencies or build commands run during
installation. Do not package executables, ELF32, or big-endian binaries.

## ABI version 1

Include `audio/plugin_api.h`, which depends only on C's `stdint.h`. Export this
symbol with default visibility:

```c
const AudioxPluginV1 *audiox_plugin_v1(void);
```

The immutable descriptor has fields in this order:

| Field | Meaning |
| --- | --- |
| `abi_version` | `AUDIOX_PLUGIN_ABI_VERSION` (1) |
| `struct_size` | `sizeof(AudioxPluginV1)` |
| `id` | Full package ID, e.g. `plugin_gain` |
| `param_count` | 0–32 |
| `params` | Ordered `AudioxParameterV1[]`: `name`, `label`, `min`, `max`, `default_value` |
| `create(sample_rate, max_frames)` | Return a new independent instance, or NULL on failure |
| `destroy(instance)` | Release instance resources |
| `reset(instance)` | Clear history without changing the instance |
| `process(instance, input, output, frames, params)` | Process mono floating-point frames |
| `tail(instance, threshold)` | Return nonzero while audible history remains above threshold |

All strings and the descriptor must remain valid for the module's lifetime.
Parameter specifications must match the package's order, names, labels, and values.
Each channel and each effect placement has an independent instance. Do not use
mutable global state for audio history. `process` writes every output sample,
handles up to `max_frames`, and consumes parameter values in descriptor order.
Do not retain audio or parameter pointers. Do not allocate, perform I/O, sleep, or
block in `process`, `reset`, or `tail`. Zero-input processing should drain effect
history; `tail` should then return zero. Effects without history always return zero.
Callbacks must not throw C++ exceptions across the C ABI.

Enabled native processing has exactly one block of latency. The first output
block is silence. At the next callback, the parent uses the preceding block's
processed output only if ready; otherwise it uses the preceding dry input.
Late processed results are discarded, never replayed. No callback waits for the
worker, and an unacknowledged request is never overwritten. After 32 callbacks
without acknowledgement the host faults and stays in delayed dry bypass until
explicit reload. Control-side polling also detects exited workers. Reset discards
pending wet/dry history and tails; its next output block is silence.

The worker requires seccomp, pidfds, `close_range`, and filesystem isolation;
unsupported systems fail closed. Landlock allows read-only module and system
library access. On stock Pi kernels without Landlock, the root worker instead
creates a private mount namespace and a read-only tmpfs root containing only
the module and system libraries, then enters it with `chroot`. No host temporary
files, devices, or `/proc` remain inside this root. This path requires mount
namespaces, tmpfs, and a mounted `/proc` during setup; Landlock is built into the
kernel, not a loadable module. Before loading module code, root workers drop to
UID/GID 65534. Both filesystem isolation paths retain seccomp; the initial
syscall policy denies network access, filesystem writes, and process creation
even in constructors. A tighter policy applies after initialization, with a
64 MiB address-space cap. Startup handshake time is bounded (1750 ms, plus bounded
cleanup). Module and library files must be readable by the unprivileged worker;
with Landlock, their parent directories must also be traversable.
Native plugins remain untrusted: CRC32 is corruption
detection, not a signature or guarantee of plugin safety.

The dynamic helper is built/staged separately from the static AudioX runtime,
along with its glibc/libstdc++ dependencies. `make plugin-examples` creates
AArch64 packages under `out/plugins/`; `make axm-pack` builds the host packer.
`make test-plugin-namespace` runs the worker regression suite in an AArch64 QEMU
guest with Landlock disabled and the bootloader's directory chroot layout,
including a missing `/tmp`, filesystem escape and mount cleanup checks.
The runtime image stages `/tmp`; the worker also creates it when absent for
compatibility with older runtime layouts, rejecting non-directory paths.

## Metadata

`axm.txt` contains printable ASCII and LF line endings. A final LF is optional.
No blank lines, comments, unknown keys, duplicate keys, or extra parameter fields
are permitted. The required keys can appear in any order:

```text
id=plugin_gain
name=Example Gain
version=1.0.0
api=1
param=gain|Gain|0|2|1
```

* `id` is `plugin_` followed by a 1–16 character slug matching
  `[a-z][a-z0-9_]*`; the full ID is at most 23 bytes.
* `name` and parameter labels are nonempty printable ASCII, at most 63 bytes.
* `version` is nonempty printable ASCII, at most 31 bytes.
* `api` must be exactly `1`.
* Up to 32 controls in total (`param` plus `text`) are allowed, including zero.
  DSP parameters use `param=name|label|min|max|default`.
  Parameter names are unique, match `[a-z][a-z0-9_]*`, and are at most 23 bytes.
  The slug grammar is the only identifier restriction: JavaScript keywords and
  inherited-property names are not additionally reserved (for example,
  `plugin_default`, parameter `default`, and parameter `constructor` are valid).
  Values are finite decimal floats (scientific notation is allowed), with
  `min < max` and `min <= default <= max`. NaN, infinity, hex, and whitespace
  in numbers are rejected.
* An optional `control=name|type|unit|step|description|choices` line after its
  `param` defines the WebUI control. Types are `float`, `integer`, `boolean`,
  and `enum`. Units are at most 23 bytes; descriptions and choices are at most
  255 bytes. Float controls require a positive step. Discrete controls require
  step 1 and integer bounds/defaults representable exactly as floats.
  Booleans have bounds 0–1. Enums use zero-based values and `~`-separated labels;
  their bounds must match the choices (at most 32). Non-enums leave choices empty.
  Without a `control` line, a parameter is a float with step 0.01 and no help.
  Plugin help and units never fall back to built-in parameter-name guesses.
* `text=name|label|default|description` adds a UI-only string (at most 255
  characters). Names are unique across numeric and text controls. Text values
  live only until the page is reloaded, survive panel/graph refreshes, and are not
  sent to DSP or saved in device configuration, MIDI mappings, or scenes.

```text
param=mode|Mode|0|2|1
control=mode|enum||1|Choose the clipping curve.|Clean~Warm~Hot
param=enabled|Active|0|1|1
control=enabled|boolean||1|Enable clipping.|
param=stages|Stages|1|8|2
control=stages|integer||1|Number of clipping stages.|
text=note|Note|My preset|Session-only note.
```

Numeric values still use the ABI's float array in `param` order; integer,
boolean, and enum values are rounded/clamped by the host, including MIDI and
scene recall. Text controls occupy no DSP array entries. There are no
audio-thread allocations for parameter updates. Rebuild modules using the
updated header when using more than eight DSP parameters.

Runtime metadata uses only fixed-size fields: `PackageMetadata.id[24]`,
`name[64]`, `version[32]`, `api`, `paramCount`, and `params[32]`, with separately
bounded text controls. Each parameter includes numeric bounds/defaults plus
control type, unit, step, description, and choices. This C++ metadata
is not a disk format or a cross-process ABI; do not serialize its raw memory.

## Binary archive format

All integers are unsigned little-endian. No alignment or padding is present:

```text
8 bytes  magic "AXMODULE"
u32      format version = 1
u32      entry count = 2..18
repeat entry count times:
  u32    kind (1=metadata, 2=module, 3=asset)
  u32    name byte length
  u64    content byte length
  u32    CRC32 of content
  bytes  ASCII name, no NUL terminator
  bytes  content
```

CRC32 uses polynomial `0xedb88320`, initial value `0xffffffff`, and final XOR
`0xffffffff` (the standard IEEE/ZIP CRC32). Entries may be in any order, but
exactly one metadata entry named `axm.txt` and one module entry named `module.so`
are required. Every name must be unique. Unknown kinds, wrong reserved names,
truncation, bad checksums, and bytes after the last entry are errors.

Limits:

| Item | Limit |
| --- | --- |
| Entire archive, including headers and names | 12 MiB |
| Metadata | 16 KiB |
| Native module | 8 MiB |
| Assets | 16 |
| Each asset | 256 KiB |
| Entry name | 63 bytes |
| Installed packages | 16 |

Asset names are flat lowercase identifiers with exactly one `.js`, `.css`, or
`.html` suffix (e.g. `index.html`, `controls_2.js`). Paths, backslashes, leading
dots, additional dots, uppercase characters, and traversal are rejected.
The ELF checks require ELF64, little-endian, current ELF version, `ET_DYN`,
a supported machine, and bounded headers/segments/sections with a load segment.
These checks are not a complete ELF linker validation.

## Storage API

`audiox::plugin` in `audio/plugin_package.hpp` provides:

* `parsePackage(data, size, package, error, requireNative=false)`
* `buildPackage(entries, bytes, error)`
* `installPackage(data, size, root, error)`
* `listPackages(root, metadataVector, error)`
* `removePackage(id, root, error)`
* `readPluginAsset(id, name, root, text, error)`

These functions return `true` on success and a diagnostic string on failure.
`kDefaultRoot` is `ROOT_MOUNT_POINT "/plugins"`; callers pass an explicit root
for tests. A missing root lists as empty without creating it. Installation creates
the root if necessary and stages files in a private directory. It syncs files and
directories, then atomically renames the staged directory to the full plugin ID,
without replacing an existing package. Explicitly delete an old package first.
Module paths are `root/id/module.so`.

All filesystem components are traversed through directory descriptors with
`O_NOFOLLOW`; package and asset files must be regular files without hard links.
Directory scans and file reads are bounded. A root lock serializes storage API
operations. Installation saves an internal `.checksums` manifest containing the
archive header and entry headers/names, without their payloads. It is not an asset.
Listing and asset access recheck the exact installed entry set, all file sizes and
CRCs, metadata, and native ELF bounds against that manifest. Corrupted packages
can be explicitly removed; removal refuses symlinks, subdirectories, and unexpected
entries rather than traversing them. After a crash, `.stage-*` directories are
ignored (but count toward the bounded directory scan); administrative cleanup may
be needed if many incomplete stages accumulate.

## Optional custom controls

A package may provide `index.html` with relative references to flat JS/CSS assets.
The host serves only package assets, not `module.so`, `axm.txt`, or `.checksums`.
Custom controls run in an opaque-origin iframe with `sandbox="allow-scripts"`
and a restrictive CSP: no network connections, external resources, form
submissions, popups, or parent DOM access. Never add `allow-same-origin`.
Script/style URLs are limited to that package's asset directory on the current
device origin. Like any untrusted webpage, a custom frame can navigate itself;
the sandbox is not a guarantee against misleading content or tracking.

The WebUI sends state on iframe load and parameter changes:

```js
// iframe -> parent
parent.postMessage({ type: 'audiox:set-param', param: 'gain', value: 1.2 }, '*');
// parent -> iframe
{ type: 'audiox:parameters', id: 'fx_1', params: { gain: 1.2 }, param_meta: [] }
```

An opaque iframe needs `'*'` as the target origin. The parent must instead check
`event.source` against the particular iframe, bind messages to its assigned effect,
and validate known parameter names and finite in-range values. It must not trust
an iframe-supplied plugin/effect ID. The iframe checks `event.source === parent`.
Standard metadata-driven parameter controls remain available without assets.

## Host regression test

Run `make test-plugin-package test-plugin-host test-plugin-integration test-audio-runtime`
for archive validation, sandbox/fault/cleanup checks, real persistent HTTP effect
lifecycle, and planar DSP/bypass/sleep/retirement regressions.

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tests/plugin_package_test.cpp src/audio/plugin_package.cpp \
  -o build/plugins/plugin_package_test
build/plugins/plugin_package_test
```

Tests cover malformed archives/ELFs, cross-architecture packing, bounds and
checksums, traversal/symlink rejection, installed corruption, install limits,
asset reads, and explicit deletion. They never load native code.
