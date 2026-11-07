# Optional telemetry

AudioX can share limited usage and reliability summaries to help improve the
project. **Telemetry is optional and off until you explicitly enable it.**
Normal audio processing, local health charts, diagnostic downloads, and updates
do not require telemetry. The public demo never collects or uploads it.

## Enable or disable telemetry

The first time you open the WebUI after a fresh installation, or after upgrading
from firmware without telemetry, a popup asks whether you want to participate:

- **Enable telemetry** starts collection on the device.
- **No thanks** saves a disabled preference and dismisses the prompt.
- **Escape** closes the prompt without choosing. Collection stays off, and the
  prompt returns on a later page load.

If you previously enabled an older telemetry format, this upgrade asks again
before collecting reports eligible for public aggregates. Earlier opt-outs
remain disabled. Old pending reports are not retroactively made public.

You can change your choice at any time:

1. Open **System** in the WebUI.
2. Find **Optional development telemetry**.
3. Turn **Enable telemetry** on or off.

The preference is saved on the Pi, not in your browser, so it applies across
browsers and survives restarts. It is not included in configuration backups.
Disabling telemetry stops collection, clears pending reports, and deletes the
installation ID. Re-enabling creates a new random ID. It cannot
recall reports already uploaded or a request already received by the collector.

## What is shared

After you enable it, AudioX samples every **10 seconds**, including while the
WebUI is closed. Idle intervals with no observed audio signal, usage events,
X-runs or configuration changes are skipped. The first summary is always kept;
CPU/temperature fluctuations alone do not keep an otherwise idle summary.
Steady audio activity is not filtered out just because settings remain unchanged.
Each retained summary contains:

| Category | Information |
| --- | --- |
| System | AudioX firmware version and Pi family |
| Installation | A persistent random 256-bit ID for grouping reports, plus the 10-second sampling interval |
| Devices | Counts of USB audio interfaces, the USB gadget, other ALSA audio cards, connected Bluetooth audio peers, input/output-capable devices, and whether the supported MIDI connection is present |
| Effects | Configured and enabled counts by built-in effect type; third-party plugins are grouped together |
| Usage | Soundboard-trigger, MIDI note/CC event, and scene-recall counts; sampler mode and whether audio activity was observed |
| Reliability | X-run counts, engine sample rate, processing block size, and the latest rounded CPU usage and temperature when available |

Enabled effects are not necessarily routed, audible, or actively processing
audio. Device counts describe audio cards and connections, not individual
channels or a complete inventory of every attached MIDI device.

**No audio, device names, Bluetooth addresses, USB vendor/product/serial
identities, stable hardware hashes, filenames, scripts, scene names, or raw
logs are sent.** The installation ID is generated randomly, not derived from
hardware or network identifiers. It links reports from this installation across
restarts, so these reports are **pseudonymous, not anonymous**. Each summary
also has a separate random ID to prevent duplicate inserts when uploads are retried.

Cloudflare hosts the collector and necessarily receives your connection IP and
browser/network metadata. The collector does not store those in its reporting
database or log request contents; it uses a transient per-IP abuse limiter.
These are privacy-minimized reports, not a guarantee of absolute anonymity.
The installation ID is visible in a ready upload preview. There is no automatic
cloud-deletion control; switching telemetry off deletes local pending data and
the local ID, not previously uploaded reports.

## Public dashboard

Anyone can view [AudioX public telemetry](https://telemetry.audiox.wklab.net/).
Enabling telemetry in this version agrees to publishing **daily aggregate
statistics**: observations, effects, broad device categories, Pi/firmware
families, reliability, and feature activity.

The public page/API never includes installation IDs, point IDs, raw reports,
exact firmware versions, per-installation filters, or exact event times.
It covers the last 30 UTC **receipt dates**, refreshes hourly, and may be empty
until reports with the new public-sharing consent arrive. Older reports
remain private and are excluded.

**With only a few participants, even daily totals can reveal individual
activity.** No minimum population threshold is enforced. Removing identifiers
does not make the aggregate page anonymous. Published information can be
copied by other people; opting out cannot recall those copies.

The charts count retained observations, not total uptime, unique users or
precise audible minutes. Offline reports can arrive on a later receipt day.
Firmware groups are broad release/prerelease/development string categories,
not trusted release verification. Voluntary reports may be incomplete or
fabricated.

## When reports upload

Reports are stored on the device until at least **100 points** are pending.
While the WebUI is open, it checks once per minute and can upload one batch of
100 through your browser to `https://telemetry.audiox.wklab.net/v1/batches`.
The computer displaying the WebUI needs internet access; the Pi does not.
You do not need a server, Cloudflare account, or port forwarding to participate.

Fewer than 100 points stay queued until enough accumulate, including across
short sessions and restarts. If the browser is closed, suspended, or offline,
delivery waits. Failed uploads retain pending data and retry later.

With continuous activity, 100 points take about **16 minutes 40 seconds** to
accumulate; idle filtering can make this take longer.

The queue holds up to **8,640 points**, approximately 24 hours of continuous
activity (longer if idle intervals are skipped). If it fills, the oldest 100 are
discarded to make room. Partial intervals across restarts and long sampling gaps
are not reported as completed intervals.

Audio activity is observed through existing level meters, approximately above
-90 dBFS, and soundboard playback state. It is not a recording or a precise
measurement of audible duration; very short signals between meter polls or
signals below that threshold can be missed.

On the System page, **Preview next upload (local only)** shows the next ready
batch without sending it. A preview is available once 100 points are pending.
Viewing the status or preview does not itself contact the collector.

## Retention and troubleshooting

The collector runs an hourly cleanup of reports received 30 or more UTC dates
ago. Cloudflare-managed backups/Time Travel and provider connection metadata
have separate retention policies; this cleanup is not immediate erasure from
those systems.

If reports do not upload, check the telemetry status on **System**, confirm
there are at least 100 points pending, and open the WebUI on a computer with
internet access. A corrupt or unreadable local queue stops collection and
upload and shows an error. Disable and re-enable telemetry to clear that queue.

Developers setting up the service or changing the report schema should use
[Telemetry collector development and deployment](../dev/telemetry).
