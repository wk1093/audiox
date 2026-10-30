# Files and HTTP interface

This page is for advanced users integrating another control surface or inspecting
saved settings. Use the [WebUI guides](./web-interface) for normal operation.
The interface is an evolving firmware API, not a versioned general-purpose
remote-control standard.

::: warning Trusted local control only
These HTTP operations control the device and can modify files, restore settings,
or reboot it. The current server does not provide a user-account/authentication
workflow or HTTPS. Do not expose it directly to the internet or an untrusted
network. Do not issue write requests casually against a working performance setup.
:::

## Persistent device files

Paths below are on the AudioX data partition, normally mounted at `/audiox`.
They are not paths on the computer running your browser.

| Path | Contents |
| --- | --- |
| `config.txt` | Active USB configuration and persistent view/playback preferences |
| `config.staging.txt` | Staged configuration saved before USB reload |
| `routing.txt` | Saved signal connections and routing-helper configuration |
| `midi_map.txt` | Sound/action/CC mappings, sampler, lighting, effects, sound order, and named macro definitions |
| `fx_params.txt` | Saved effect parameter data used by the firmware |
| `volumes.txt` | Saved gains keyed by audio thing ID |
| `bluetooth/paired.txt` | Saved Bluetooth pairing records, Auto-connect flags, and sensitive key material |
| `sfx/` | Soundboard audio files |

Use the normal editors instead of hand-editing files where possible: edits can
need validation, runtime reload, and corresponding LED/graph refresh. Backups
include the supported settings and sound roots, not arbitrary files placed on
the data partition. See [Config and backups](./config).

Do not confuse `/audiox/config.txt` with `/boot/config.txt`. The latter controls
Raspberry Pi boot behavior, kernel selection, and overlays. Changing boot files
can make the device unbootable.

## Request conventions

Base URLs in this page are relative to the device, such as
`http://audiox.local/api/system/info`.

- Read endpoints commonly return JSON or plain text; check `Content-Type`.
- Mutation endpoints commonly accept plain `key=value&key=value` text, **not**
  JSON. Conventions differ by endpoint; copy the current WebUI's request shape
  when implementing an advanced client.
- Treat non-2xx status codes as failures and read the accompanying text.
- A queued trigger response is not a completion receipt.
- Poll small status endpoints at sensible intervals instead of repeatedly
  downloading files or rebuilding state.
- Use exact IDs from the device. Friendly names are not stable identifiers.

For example, these **read-only** requests inspect the running device:

```sh
curl --fail http://audiox.local/api/system/info
curl --fail http://audiox.local/api/routing/things
curl --fail http://audiox.local/api/audio/volumes
curl --fail http://audiox.local/api/effects
curl --fail http://audiox.local/api/macros
```

Replace the hostname with your numeric device address if name resolution is
unavailable. The commands require access to your device, not this docs site.

## Audio, routing, and effects

| Endpoint | Purpose |
| --- | --- |
| `GET /api/audio/devices` | Detected audio device information |
| `POST /api/audio/rescan` | Request device rescan |
| `GET /api/routing/things` | Routing nodes, channels, IDs, and current graph information |
| `POST /api/routing/reload` | Rebuild runtime routing from saved configuration |
| `POST /api/routing/helper/create` or `/delete` | Create/remove the routing helper |
| `GET /api/audio/volumes` | Audio thing IDs/names and current gains |
| `POST /api/audio/volume/set` | Set and save a gain using `id` and `gain` |
| `GET /api/effects` | Existing effects, parameter metadata, and MIDI assignments |
| `POST /api/effect/create`, `/set`, `/delete` | Manage an effect or its parameters |
| `POST /api/effect/midi_cc/set`, `/delete` | Effect parameter CC mapping |
| `POST /api/effect/midi_toggle/set`, `/delete` | Effect bypass-note mapping |
| `POST /api/effect/midi_light/set`, `/delete` | Effect enabled/bypassed lighting |

HTTP volume changes use **gain**, with a 0.0-2.0 API/runtime range; the WebUI
slider itself stops at 100% (unity), while MIDI CC can reach 200%.
Lua's `volume.set` uses **percent**, restricted to 0-100. Do not send a percentage
as an HTTP gain or assume a Lua parameter is normalized.

See [Routing](./routing) for connection syntax and graph limitations, and
[Effects](./effects) for the actual parameter units.

## Soundboard and MIDI

| Endpoint | Purpose |
| --- | --- |
| `GET /api/soundboard/order` | Complete ordered WAV filename list, one per line |
| `POST /api/soundboard/order` | Save a complete newline-separated order; stale lists can return 409 |
| `POST /api/soundboard/trigger/<target>` | Start a one-shot sound |
| `POST /api/soundboard/press/<target>` | Start the shared held voice |
| `POST /api/soundboard/release/<target>` | Release the held voice |
| `POST /api/soundboard/stop_all` | Stop soundboard playback |
| `POST /api/soundboard/reload` | Reload clips and clean up missing-file mappings |
| `GET /api/soundboard/modes`, `POST /api/soundboard/mode/set` | Inspect/set per-sound playback mode |
| `GET /api/midi/last_note`, `/api/midi/last_cc` | Latest input values, sequence counters, and connection status for learning |
| `GET /api/midi/mappings`, `POST /api/midi/mapping/set`, `/delete` | Sound note bindings |
| `GET /api/midi/actions`, `POST /api/midi/action/set`, `/delete` | Stop All / Sampler Toggle mappings |
| `GET` / `POST /api/midi/sampler_config` | Sampler keyboard enable, channel, and root note |
| `GET` / `POST /api/midi/light_config` | Global LED output channel and state velocities |
| `GET /api/midi/light_sounds`, `POST /api/midi/light_sound/set`, `/delete` | Per-sound light overrides |
| `POST /api/midi/light_refresh` | Resend current LEDs |
| `GET` / `POST /api/midi/light_dim` | Inspect/change temporary dim state |
| `GET /api/midi/cc_volumes`, `POST /api/midi/cc_volume/set`, `/delete` | Volume CC assignments |
| `GET` / `POST /api/framebuffer/view` | Persistent meters/soundboard display choice |

Soundboard trigger targets use the server's target encoding; the current WebUI
encodes its filename using `encodeURIComponent` before appending it to the path.
Keep filenames simple, and do not assume arbitrary filesystem paths are valid
trigger targets.

Per-sound lighting mutation fields are `sfx`, `mapped_vel`, and `playing_vel`.
Inspect the current API behavior before depending on clearing individual
overrides. The WebUI currently exposes global lighting and effect/macro lighting,
not a per-sound lighting form.

## Named macros

| Request | Purpose |
| --- | --- |
| `GET /api/macros` | Return `{ "macros": [...] }` including IDs and runtime errors |
| `POST /api/macros` | Create or update a saved definition |
| `DELETE /api/macros` | Remove by `id` |
| `POST /api/macros/run` | Queue the saved macro using `id` |

A create/update body contains:

```text
id=<existing ID, omitted for create>
name_hex=<UTF-8 name encoded as hex>
midi_enabled=0 or 1
channel=0..15
note=0..127
keys=<plain chord, empty for script>
script_hex=<UTF-8 Lua source encoded as hex, empty for chord>
idle_velocity=0..127
pressed_velocity=0..127
```

Fields can be newline-separated or joined with `&`. Chord `+` separators are
literal plus signs: do not run the whole body through a form encoder that changes
them. Names and script source are hex to keep punctuation and UTF-8 separate
from field separators. Delete/Run use `id=<positive integer>`.

The list includes plain `name`, boolean `midi_enabled`, and hex-encoded
`script_hex` / `runtime_error_hex`. Decode error text as text, not HTML.

Saving syntax-valid Lua does not guarantee every branch will execute
successfully. Device requirements and actual effect/volume targets are checked
at runtime; see [Macros](./macros) and the [Lua reference](/lua/api).

## Bluetooth, configuration, and system

| Endpoint | Purpose |
| --- | --- |
| `GET /api/bluetooth/devices` | Saved/nearby Bluetooth devices and current status |
| `POST /api/bluetooth/scan`, `/pair`, `/connect`, `/disconnect`, `/unpair`, `/auto-connect` | Bluetooth lifecycle and per-device Auto-connect operations |
| `POST /api/config/reload` | Apply staged USB configuration |
| `GET /api/config/backup` | Stream a complete `.axb` backup |
| `PUT /api/config/restore` | Upload/validate/stage an `.axb`, then reboot |
| `GET /api/system/info`, `/api/version` | Device/build information |
| `GET /api/system/logs` | Bounded, filtered log query |
| `POST /api/system/sync`, `/restart`, `/shutdown` | System lifecycle actions |
| `GET` / `DELETE /api/system/boot-failure` | Read/dismiss saved failed-boot report |
| `PUT /api/update` | Install a board-matching `.axu` firmware archive |

`/api/rootfs/<path>` reads/writes/deletes supported files under the data root;
it is not a general shell or access to the entire live root filesystem.
Sound uploads through this interface run conversion and cache-management logic.

Low-level boot/kernel/runtime upload endpoints also exist for development.
They are not the recommended update path. Use the validated `.axu` workflow
described in [System](./system), and the [development guide](./development)
when working on boot components.

## Firmware archive format

AXU is a little-endian binary archive, not a ZIP file. Its header contains
eight-byte `AXUPDATE` magic, a `u32` format version (`2`), and a `u32` entry
count (1-64). Each entry has a `u32` type, `u32` CRC32, `u64` payload size, then
the payload bytes. Entries may be ordered independently, and no bytes may follow
the final entry. Types 1-5 are required exactly once in a v2 archive; type 6 is
optional and may appear at most once.

| Type | Payload |
| --- | --- |
| 1 | Boot `config.txt` |
| 2 | Bootloader `initramfs.cpio.gz` |
| 3 | Kernel image |
| 4 | Runtime `program.cpio.gz` |
| 5 | `key=value` metadata |
| 6 | Optional single-line `cmdline.txt`, containing `rdinit=/init` |

Metadata requires `board` (`pi4` or `pi5`) and `kernel` (a boot filename matching
`kernel*.img`). Optional `version` and `kernel_version` identify the build.
The board must match the device, and the boot config's `kernel=` must name the
archive kernel. Uploads are limited to 256 MiB.

For format extensions, add entry types or metadata keys rather than changing
existing meanings. Unknown metadata keys are ignored. Unknown entry types are
CRC-checked and skipped unless bit 31 is set, which requires the reader to
understand that entry. Missing optional entries leave the existing file intact.
A header-version bump is for changes that cannot be expressed compatibly.

The implementation also accepts the four-entry v1 format without metadata and
installs its kernel as `kernel8.img`. Firmware at 1.9.0 or earlier has stricter
entry-count rules; see [Development](./development#build-and-install-an-axu)
before deploying to it.
