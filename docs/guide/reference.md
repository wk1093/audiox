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
| `config/config.txt` | Active USB configuration and persistent view/playback preferences |
| `config/config.staging.txt` | Staged configuration saved before USB reload |
| `config/routing.txt` | Saved signal connections and routing-helper configuration |
| `config/midi_map.txt` | Sound/action/CC mappings, sampler, lighting, effects, and sound order |
| `config/scripts.txt` | Named script definitions, trigger bindings, and permissions |
| `config/scripts/` | Script-owned persistent options and custom files |
| `config/scenes/` | Named setup scenes |
| `config/fx_params.txt` | Saved effect parameter data used by the firmware |
| `config/volumes.txt` | Saved gains keyed by audio thing ID |
| `config/bluetooth/paired.txt` | Saved Bluetooth pairing records, Auto-connect flags, and sensitive key material |
| `sfx/` | Soundboard audio files |
| `plugins/` | Installed native `.axm` modules, metadata, and sandboxed UI assets; excluded from normal backups |
| `config/telemetry-consent-v3.txt` | Device-local opt-in decision (`0` or `1`) for linkable reports and public daily aggregates; excluded from backups |
| `config/telemetry-queue-v3.txt` | Bounded pending 10-second usage reports with public-sharing consent; excluded from backups |
| `config/telemetry-installation-id.txt` | Random 256-bit installation ID; deleted on opt-out; excluded from backups |

Use the normal editors instead of hand-editing files where possible: edits can
need validation, runtime reload, and corresponding LED/graph refresh. Backups
include the supported settings and sound roots, not arbitrary files placed on
the data partition. See [Config and backups](./config).

Do not confuse `/audiox/config/config.txt` with `/boot/config.txt`. The latter controls
Raspberry Pi boot behavior, kernel selection, and overlays. Changing boot files
can make the device unbootable.

## Request conventions

### Optional telemetry

See [Optional telemetry](./telemetry) for the privacy scope and user controls,
and [Telemetry collector setup](../dev/telemetry) for deployment details.

| Endpoint | Purpose |
| --- | --- |
| `GET /api/telemetry` | Consent (`unknown`, `disabled`, `enabled`), consent version, pending count, capacity, interval, batch size and error |
| `POST /api/telemetry` | Exact text `enabled=0` or `enabled=1&consent_version=3`; opting out clears queued points and installation ID |
| `GET /api/telemetry/batch` | Next 1-1,000 points, or `{}` when empty/disabled; non-2xx on a queue error. Browser upload cycles start at 100 pending points |
| `POST /api/telemetry/ack` | Exact 65-character `batch` token from a successfully uploaded batch; rejects stale tokens |

These are local device endpoints. No GET request causes a cloud upload.
The browser relays ready batches only after opt-in. Consent is not copied in
configuration backups.

For plugin upload, catalog, reload/removal, and UI asset routes, see
[Native plugin HTTP management](../plugins#install-and-use).

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
| `GET /api/effects` | Existing effects, display `name`, saved `custom_name`, parameter metadata, and MIDI assignments |
| `POST /api/effect/create`, `/set`, `/delete` | Manage an effect or its parameters |
| `POST /api/effect/name` | Save `id` and `name_hex` (UTF-8 encoded as hex, up to 63 decoded bytes); empty name restores automatic naming |
| `POST /api/effect/midi_cc/set`, `/delete` | Effect parameter CC mapping |
| `POST /api/effect/midi_toggle/set`, `/delete` | Effect bypass-note mapping |
| `POST /api/effect/midi_light/set`, `/delete` | Effect enabled/bypassed lighting |

HTTP volume changes use **gain**, with a 0.0-2.0 API/runtime range; the WebUI
slider and MIDI CC both span 0-200%, with 100% at unity and 200% at 2x gain.
Lua's `volume.set` uses **percent**, restricted to 0-100. Do not send a percentage
as an HTTP gain or assume a Lua parameter is normalized.

See [Routing](./routing) for connection syntax and graph limitations, and
[Effects](./effects) for the actual parameter units.

## Soundboard and MIDI

| Endpoint | Purpose |
| --- | --- |
| `GET /api/soundboard/order` | Complete ordered WAV filename list, one per line |
| `POST /api/soundboard/order` | Save a complete newline-separated order; stale lists can return 409 |
| `GET /api/soundboard/layout` | Read list/grid mode, dimensions, learned MIDI corners, and cell assignments |
| `POST /api/soundboard/layout` | Set `mode`, `rows`/`columns`, a `corner`/`note`, or an `sfx`/`row`/`column` assignment (occupied cells swap); use `action=stop_all` or `action=sampler_toggle` instead of `sfx` for action tiles. `unassign=1` clears the element's cell and MIDI bindings |
| `GET /api/midi/sampler_mode` | Read runtime sample-selection mode (`active`) |
| `POST /api/midi/sampler_mode` | Toggle runtime sample-selection mode, also used by the sampler grid tile |
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
overrides. The WebUI currently exposes global lighting and effect/script lighting,
not a per-sound lighting form.

## Named scripts

| Request | Purpose |
| --- | --- |
| `GET /api/scripts` | Return `{ "scripts": [...] }` including IDs and runtime errors |
| `POST /api/scripts` | Create or update a saved callback script |
| `DELETE /api/scripts` | Remove by `id` |
| `POST /api/scripts/run` | Queue the saved script's `run` hook using `id` |
| `POST /api/scripts/control` | Persist a declared option or queue a declared button |

A create/update body contains:

```text
id=<existing ID, omitted for create>
name_hex=<UTF-8 name encoded as hex>
midi_enabled=0 or 1
channel=0..15
note=0..127
script_hex=<UTF-8 callback source encoded as hex, up to 17,408 decoded bytes>
config_write_ack=0 or 1
idle_velocity=0..127
pressed_velocity=0..127
```

Fields can be newline-separated or joined with `&`.
Names and script source are hex to keep punctuation and UTF-8 separate
from field separators. Delete/Run use `id=<positive integer>`.

The list includes plain `name`, boolean `midi_enabled`, derived `event_mask`,
`hook_mode: true`, saved `config_write_allowed`, and hex-encoded
`script_hex` / `runtime_error_hex`. The `ui` array describes native controls:
`key`, `label`, `type` (`boolean`, `number`, `string`, `button`), typed `value`,
and optional numeric `min`/`max`. `ui_error_hex` reports per-script
schema/storage failures without preventing source editing. Decode error text
as text, not HTML.
Saving inspects unconditional top-level `hook()` registrations without running
handlers. Run is rejected without a `run` hook. Device-management/configuration permission
requires explicit acknowledgment. `/api/macros` and `/api/midi/chords` remain
available for compatibility. `/api/macros` retains its `macros` response
property.

Control requests use `id=<positive integer>&key_hex=<UTF-8 key as hex>`.
Options also require `value_hex=<hex-encoded scalar text>`: `true`/`false`,
a finite decimal number, or UTF-8 string according to the declared type.
Buttons omit `value_hex`. Unknown keys, invalid values, unavailable scripts,
and full trigger queues are errors. Options persist before queuing
`config_change`; buttons queue `ui`. Responses acknowledge saving/queuing,
not callback completion. Script-owned options need no official-device write
permission.

Saving syntax-valid Lua does not guarantee every branch will execute
successfully. Device requirements and actual effect/volume targets are checked
at runtime; see [Scripts](./macros) and the [Lua reference](/lua/api).

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

The reader also accepts the four-entry v1 format without metadata and installs
its kernel as `kernel8.img`.
