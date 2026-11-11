# LLVM bitcode effect plugins

An `.axm` is an AudioX Module package containing a mono effect as LLVM bitcode,
metadata, and optional HTML/CSS/JavaScript controls. The audio worker compiles the
bitcode to a private native image and executes it directly in its audio graph.
The HTTP/control process never loads or executes plugin code. Installation only
validates package framing, checksums, metadata, and bitcode magic.

**Breaking format change:** format-1 packages and ELF `.so` modules are no longer
supported. Rebuild them as format-2 packages containing `module.bc`; legacy
uploads produce an explicit rebuild-required error. Previously installed format-1
packages are automatically deleted during catalog scans (including startup), with
a log entry, so they cannot block the Plugins list. Saved effects and parameters
are retained as unavailable/bypassed until you install a rebuilt package and reload
it. This cleanup does not delete corrupt current-format packages or unknown files;
unsafe directory contents produce an explicit error instead.

## Install and use

On **Effects**, choose an `.axm` in **Plugins** and select **Install plugin**.
The Routing **Add → plugin** menu then offers its stereo effect. Unnamed instances
use the package's declared name and instance number; custom names override it.
Parameters use the existing
sliders, MIDI mappings, and Lua effect API, with stable `fx_N` instance IDs.
WebUI numeric parameter updates preserve native float precision; values are not
quantized to four decimal places. Custom controls are optional and open only when
explicitly requested.

Use **Reload** to recreate plugin instances. Plugin crashes and hangs affect the
whole audio worker, not a per-plugin process. The supervisor restarts that worker
with backoff and disables **all** plugins after consecutive failures, showing a
warning. Disabled plugins dry-bypass while built-in processing remains available.
Effect cards report worker loading, readiness, and compilation/creation errors;
metadata validation in the main program alone does not mean processing is ready.
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
device performance still needs physical testing.

Management HTTP endpoints:

| Method | Endpoint | Body/result |
| --- | --- | --- |
| GET | `/api/plugins` | Installed IDs, metadata, parameter ranges, UI availability |
| PUT | `/api/plugins/install` | Binary `.axm`, nonempty and at most 12 MiB |
| POST | `/api/plugins/remove` | `id=plugin_gain`, rejects references from live/saved effects |
| POST | `/api/plugins/reload` | `id=plugin_gain`, prepares replacement instances |
| GET | `/api/plugins/plugin_gain/assets/index.html` | Validated flat assets with sandbox CSP |

Installation is a trusted-local administrative operation, not a signed-package
marketplace. Do not expose the existing unauthenticated HTTP interface publicly.

## Build an example

From the repository root (Clang matching the device compiler's LLVM major,
a C++17 compiler, and Linux headers are required):

```sh
mkdir -p build/plugins
c++ -std=c++17 -O2 -Iinclude tools/plugins/axm-pack.cpp src/audio/plugin_package.cpp \
  -o build/plugins/axm-pack
clang -std=c11 -O2 -fPIC -fno-stack-protector -emit-llvm -c -Iinclude \
  examples/plugins/gain/gain.c -o build/plugins/gain.bc
build/plugins/axm-pack examples/plugins/gain/axm.txt build/plugins/gain.bc \
  build/plugins/gain.axm examples/plugins/gain/index.html \
  examples/plugins/gain/controls.js examples/plugins/gain/style.css
clang -std=c11 -O2 -fPIC -fno-stack-protector -emit-llvm -c -Iinclude \
  examples/plugins/softclip/softclip.c -o build/plugins/softclip.bc
build/plugins/axm-pack examples/plugins/softclip/axm.txt \
  build/plugins/softclip.bc build/plugins/softclip.axm
```

The packer refuses to overwrite its output. Delete an old `.axm` explicitly before
repacking. For an AArch64 device, add
`--target=aarch64-linux-gnu --gcc-toolchain=/usr` to Clang (with the cross headers
installed). Bitcode remains architecture-specific: its target triple must match
the audio worker. The worker's separate graph compiler validates LLVM IR,
architecture, the ABI entry point, and all external references; incompatible
LLVM versions or unresolved imports fail explicitly. No build commands execute
during installation. Packages must not contain ELF objects or shared libraries.

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

Plugin callbacks execute synchronously inside the audio worker, with no
additional per-plugin IPC block or initial silent block. `reset` clears instance
history before the next block; `tail` receives the requested threshold. The
worker prepares and destroys code/instances on its control thread, outside graph
processing; instance retirement waits for the audio thread to release its graph.

Native images have separate executable and writable pages, never writable
executable mappings. Global constructors/destructors, thread-local globals, and
unresolved external symbols are rejected. Supported imports are C allocation,
memory routines, common scalar math functions, and `abort`; there is no dynamic
library loading or arbitrary host symbol lookup. C++ plugins should use
`-fno-exceptions -fno-rtti -fno-stack-protector` and avoid runtime initialization.
This is crash isolation, **not a security sandbox**: plugins execute native code
and must come from trusted authors. CRC32 detects corruption, not authenticity.

`PluginHost` defaults to control-only mode. Only the exec'd audio worker calls
`audiox::plugin::setPluginExecutionEnabled(true)` before preparing hosts. The main
process can retain metadata handles and saved values without executing callbacks.
Setting the process-local flag false bypasses all plugin processing.
The graph compiler is a short-lived compilation helper, not a plugin process;
it never invokes plugin callbacks. LLVM is not linked into the main/audio worker.

`make plugin-examples` creates AArch64 packages under `out/plugins/`;
`make axm-pack` builds the host packer. `make test-plugin-host` exercises compiled
bitcode, same-block processing, channel independence, reset/tail, metadata/ABI
mismatch, unresolved imports, invalid bitcode, and process-level crash isolation.

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
u32      format version = 2
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
exactly one metadata entry named `axm.txt` and one module entry named `module.bc`
are required. Every name must be unique. Unknown kinds, wrong reserved names,
truncation, bad checksums, and bytes after the last entry are errors.

Limits:

| Item | Limit |
| --- | --- |
| Entire archive, including headers and names | 12 MiB |
| Metadata | 16 KiB |
| LLVM bitcode module | 8 MiB |
| Assets | 16 |
| Each asset | 256 KiB |
| Entry name | 63 bytes |
| Installed packages | 16 |

Asset names are flat lowercase identifiers with exactly one `.js`, `.css`, or
`.html` suffix (e.g. `index.html`, `controls_2.js`). Paths, backslashes, leading
dots, additional dots, uppercase characters, and traversal are rejected.
The archive parser checks raw/wrapped LLVM bitcode magic. Full LLVM verification
and native architecture validation occur only in the worker's compiler helper.

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
Module paths are `root/id/module.bc`. The compatibility `requireNative` argument
to `parsePackage` is retained but ignored; native compatibility requires LLVM.

All filesystem components are traversed through directory descriptors with
`O_NOFOLLOW`; package and asset files must be regular files without hard links.
Directory scans and file reads are bounded. A root lock serializes storage API
operations. Installation saves an internal `.checksums` manifest containing the
archive header and entry headers/names, without their payloads. It is not an asset.
Listing and asset access recheck the exact installed entry set, all file sizes and
CRCs, metadata, and bitcode magic against that manifest. Corrupted packages
can be explicitly removed; removal refuses symlinks, subdirectories, and unexpected
entries rather than traversing them. After a crash, `.stage-*` directories are
ignored (but count toward the bounded directory scan); administrative cleanup may
be needed if many incomplete stages accumulate.

## Optional custom controls

A package may provide `index.html` with relative references to flat JS/CSS assets.
The host serves only package assets, not `module.bc`, `axm.txt`, or `.checksums`.
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
for archive validation, worker-local compilation/fault checks, real persistent HTTP effect
lifecycle, and planar DSP/bypass/sleep/retirement regressions.

```sh
c++ -std=c++17 -Wall -Wextra -Werror -Iinclude \
  tests/native/plugins/plugin_package_test.cpp src/audio/plugin_package.cpp \
  -o build/plugins/plugin_package_test
build/plugins/plugin_package_test
```

Package tests cover malformed archives, legacy ELF rejection, bounds and
checksums, traversal/symlink rejection, installed corruption, install limits,
asset reads, and explicit deletion. Package tests never load native code;
worker-local loader tests deliberately opt in and execute compiled fixture code.
