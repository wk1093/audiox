# System and maintenance

Use **System** for device information, filesystem and power actions, runtime
logs, and firmware updates. Configuration backups and restores are on
**Config**; see [Config and backups](/guide/config) before maintenance.

::: warning Before changing firmware or power state
Save pending edits, stop critical audio work, reduce monitoring levels, and download a configuration backup to another machine. Restart, shutdown, USB reconfiguration, and firmware installation interrupt audio. Keep power and the data cable stable until an update has completed.
:::

The HTTP API has no authentication or TLS and runs on port 80. Anyone who can
reach it can administer the device. See [Networking](/guide/networking) before
adding bridges, forwards, or other interfaces.

## Device Info

For the optional consent prompt, background collection, upload preview and
privacy details, see [Optional telemetry](./telemetry). Local health
charts and diagnostic downloads do not require enabling remote telemetry.

The card shows:

| Field | Meaning |
| --- | --- |
| audiox | Firmware version reported by the running runtime |
| Kernel | Running kernel identity |
| Raspberry Pi | Hardware model; the update API also reports a machine-readable board |
| CPU Clock | Reported current CPU frequency in MHz, or a dash if unavailable |
| CPU Temperature | Live CPU temperature in °C, refreshed while System is open; its meter shifts from cool green through yellow to red as it warms |
| Uptime | Time since kernel boot, displayed in hours/minutes or days |
| Memory | Used/total MB, calculated using available memory. The meter turns yellow as usage rises and red near capacity. |
| Storage | Used/total persistent storage in GiB or MiB, with free space and filesystem-reserved space below. The bar includes used and reserved space, so its remaining portion matches space available for uploads; it shifts from green to yellow as space runs low and red when nearly full. This excludes the boot partition and RAM. |
| Load (1m) | One-minute system load average, with a meter scaled against the number of online CPUs. This is a relative load indicator, not a CPU-utilization percentage. |
| CPU Utilization | Current CPU busy-time percentage calculated from `/proc/stat` counter changes between visible System refreshes; a fresh reading appears after the next sample. |
| Audio Buffer | Current target ALSA period, negotiated period/buffer sizes, active ALSA stream count, cumulative X-runs, and (while above base) the quiet interval and failed-downshift count. The DSP block size remains fixed. The displayed duration estimates the largest single ALSA buffer's capacity at the engine rate; it is **not measured end-to-end or output latency**. |
| FFmpeg | Available converter's reported version/status |

System refreshes while the tab is active and visible. A runtime-only watchdog restart does not reset kernel uptime; a full reboot does. Record the version/kernel/model alongside logs when reporting a fault.

### Recent health and audio devices

Open **Health and Logs** for telemetry, diagnostics, stress tests, and logs.
Device information, actions, and firmware updates remain on the main page.

**Power and Cooling** distinguishes current undervoltage/throttling flags from
events recorded since boot. The combined throttling/thermal-limit flag does
not prove temperature was the cause; undervoltage can also throttle a Pi.
Unavailable telemetry is not treated as healthy. On Pi 5 the fan status shows
RPM, the current cooling step, and whether the saved custom curve matches the
booted device tree. The fan dialog shows saved and active thresholds separately.
A stall warning requires a sustained cooling request with zero RPM; stopping
below the first threshold is normal.

**Recent Health** is sampled once per second by the device's main loop, even
without a browser connected. A fixed 600-entry ring buffer retains the last
10 minutes of readings and overwrites the oldest entry in constant time.
Both the device and browser discard samples aged 10 minutes or older, including
across sampling gaps. The chart's time axis stays fixed at 10 minutes.
Temperature (degrees C) and CPU utilization (%) use the left axis;
X-runs between consecutive samples use the independently scaled right axis.
Missing readings and interrupted sampling are gaps, not interpolated
measurements. History resets when the runtime restarts; counter resets invalidate
only the corresponding interval.

`GET /api/system/history?limit=600` returns recent samples in chronological
order (default 600). Limits of 1–1,000 remain accepted for compatibility, but
no more than 600 samples are retained. The response contains `runtime_id`,
`sample_interval_ms`, `capacity`, `retention_ms`, `now_ms`, and `samples`. Each sample contains
`id`, monotonic `now` (milliseconds), `temperature`, `cpu`, and interval `xruns`;
unavailable values are `null`. Requests never add samples. The browser fills the
chart with up to 600 readings on opening, then merges the latest 50 by sample ID.
After a long gap it refills history; a new runtime ID discards old browser data.

**Audio Device Health** shows detected ALSA devices and routed stream state,
the actual negotiated sample rate, MMAP or RW transfer mode, period/buffer
sizes, and per-stream X-runs since stream setup. Stream telemetry is published
without audio-thread locks every half second; graph rebuilds recreate stream
counters. Devices seen and then disconnected remain listed for the browser
session. A connected device without a routed stream is not reported as broken.

**Download Diagnostic Summary** exports system metrics and bounded log
classification counts (at most 128 lines and 64 KiB per stdout/stderr source).
It intentionally omits raw log messages, device identifiers/names, configuration,
pairing records/keys, scripts, and audio files rather than attempting unreliable
redaction of arbitrary log text. Missing logs are marked unavailable. Use the
normal log viewer separately when you need details, and review private logs
before sharing them.

### Fan curve

Open **Fan curve** in the CPU Temperature card to configure the four automatic speed steps. Drag the graph points or focus one and use the arrow keys (Shift + arrow adjusts speed more finely); temperature thresholds must increase and stay farther apart than the hysteresis. A fan must be connected to the Pi 5 fan header; this does not control USB, GPIO, or case-specific fans.

On a Pi 5, the graph edits thresholds (40–80 °C), PWM speeds (1–255), and shared hysteresis (1–15 °C). Saving writes a managed `dtparam=fan_tempN` block to boot `config.txt`; firmware applies it after restart. Disabling the custom curve removes only AudioX's managed block and leaves other boot settings intact.

Pi 5 images include the `pwm-fan` kernel driver in both bootloader and runtime
initramfs files; the RP1 PWM controller is built into the supplied kernel.
The board device tree already defines the header fan and thermal trips, so
no additional overlay is needed. Firmware detects a connected fan at boot.
After installing an image with these drivers, save the curve and **Restart
Device** (not just the runtime). Below the first threshold the fan can remain
stopped. If it still does not respond above that threshold, check the header
connection and kernel logs for `pwm-fan` probe errors; with the debug shell,
check for a `pwmfan` entry in `/sys/class/hwmon/hwmon*/name`.

On Pi 4, the dialog displays a **preview-only warning** and the graph remains interactive for UI testing, but saving is disabled because Pi 4 does not have the Pi 5 fan connector/firmware fan-curve control.

After five X-runs within 30 seconds, AudioX temporarily increases the target
ALSA period and reopens affected PCM streams. It attempts one step back after
30 seconds without an X-run, or 60 seconds after a burst of five. If a
downshift is followed by enough X-runs to return to the previous level within
10 minutes, AudioX considers that downshift unsuccessful and waits 5 minutes
before the next attempt; repeated unsuccessful attempts increase the wait to
10 and then 15 minutes. This tracking is in-memory and resets when the runtime
restarts. A notification accompanies an increase.
The first 60 seconds after runtime startup are excluded from adaptive decisions
and health warnings. Buffer increases and PCM reopens normally have a
10-second settling period before another increase. After a downshift, a
one-second settling period follows the last successful reopen; five subsequent
X-runs can immediately restore the previous level and count as a failed
downshift. X-runs during settling postpone the next downshift even when they do
not trigger an increase. All X-runs remain included in the diagnostic total,
including startup and settling events. ALSA close failures include the stream,
device path, pre-close state, and error code in the runtime log.

Playback is primed before starting or restarting: at the base hardware period
it still starts with one DSP block; larger hardware periods queue up to one
buffer minus one period before starting, so the extra capacity provides actual
scheduling headroom. The RW and MMAP paths use the same startup threshold.
The host test suite verifies policy behavior, but the effective X-run reduction
and audible latency tradeoff still require testing with the intended Raspberry Pi,
devices, and workloads. Larger buffers do not resolve sustained CPU overload.
ALSA buffer capacity is headroom available to the PCM stream; actual latency
also depends on period scheduling, capture and playback paths, USB/device
buffering, and the host.

### Optional audio stress controls

The System page includes stress controls only when the build's
`AUDIO_STRESS_TEST` menuconfig option is enabled. Normal builds default this
option off. The audio-thread test adds a bounded amount of busy work to each
audio block (1–20 ms) for up to 30 seconds; the CPU contention test runs workers
on all available cores, capped at eight, for up to 30 seconds. Only one test can
run at a time, and either can cause audible dropouts or adaptive-buffer changes.
Use **Stop Test** to end early. These tests are for controlled diagnostics, not
normal operation or sound-quality evaluation.

FFmpeg availability matters for [Soundboard](/guide/soundboard) preprocessing. It is not a statement that every source media format will decode.

## Device actions

### Sync Filesystem

1. Save pending changes in the relevant UI page first.
2. Click **Sync Filesystem**.
3. Wait for the completion notification.

This invokes `sync()` to flush filesystem writes. It does **not** click Save in another page, commit unsaved editor text, or make it safe to interrupt a running update.

### Restart Device

1. Save work and stop audio that must not be interrupted.
2. Click **Restart Device** and confirm the dialog.
3. Leave the page open while the loading dialog checks for a new boot.
4. After the UI reconnects/reloads, check System version and uptime, then reopen host audio/MIDI streams if needed.

The server replies, syncs, then requests a reboot. The browser probes readiness about once a second and checks uptime for evidence of a new boot; a brief cable or network outage alone is not treated as restart success.

**Stop waiting** closes the browser's reconnect wait. It does not cancel the restart or installation already requested on the device.

### Shutdown Device

1. Save work, then click **Shutdown Device**.
2. Confirm the interruption.
3. Wait for the device to halt before disconnecting power or removing its card.

The UI stops its normal connection polling after an accepted shutdown request. A successful response means shutdown was requested, not that power has already disappeared. Do not pull power merely because the HTTP response arrived. To start again, restore power using the board's normal procedure.

## Logs

The System log pane queries recent **current runtime stdout/stderr**; it is not a complete system journal or kernel-log viewer.

### Filtering and navigation

1. Select **Source**: `stdout + stderr`, `stdout only`, or `stderr only`.
2. Select **Level**:
   - **all**: every line.
   - **error only**: errors.
   - **warn + error**: warnings and errors.
   - **info + warn + error**: excludes debug.
   - **debug + info + warn + error**: includes debug.
3. Enter a **Text Filter** for a case-insensitive substring, not a regular expression. It is limited to 160 UTF-8 bytes.
4. Set **Max Lines** from 1 to 5,000; the default is 600.
5. Read the summary for omitted older matches, a bounded-tail search limit, or missing source files.

Severity is inferred from message text (`error`/`[ERR]`/`fatal`, warnings, debug; otherwise info), not from which stream it uses. An error can be on stdout.

| Control or gesture | Result |
| --- | --- |
| Refresh Logs | Re-enables following and requests a fresh view |
| Pause live logs / Resume live logs | Stops/resumes live updates without clearing device logs |
| Clear Filter | Resets source to both, level to all, text to empty, limit to 600; resumes following |
| Scroll/wheel upward, Home/PageUp/ArrowUp | Pauses following so new lines do not pull you to the bottom |
| Select text | Defers display replacement while selecting/copying |
| Change a filter | Resumes following and requests the new query after a short debounce |

There is **no log-delete action** in this toolbar. Clear Filter does not erase logs.

### Why a search can miss older lines

The backend scans at most the last 2 MiB per source, clips returned lines to 2,048 bytes, and caps the response at 512 KiB. Max Lines is an upper bound, not a guarantee of that many rows. Combined stdout/stderr is not a timestamp-synchronized journal.

Long lines are marked as clipped. A bounded-tail result with no matches does not
prove the event never happened.

For scripted reads:

```bash
curl --fail --show-error \
  'http://audiox.local/api/system/logs?limit=600&source=both&level=warn&text=USB'
```

### Preserve evidence before more restarts

Runtime startup rotates nonempty `stdout.log` and `stderr.log` into `.previous` copies retaining up to the last 1 MiB each, then starts fresh files. Empty old logs do not erase nonempty previous evidence. **Current runtime logs remain unbounded**, so sustained noisy logging can fill storage and wear the card.

Kernel capture is separate:

- `kernel.log` and `kernel.log.previous`: bounded at 256 KiB each, read from `/dev/kmsg`. Headers identify the kernel/boot/runtime.
- `pstore.log` and `.previous`: bounded copies of existing persistent crash records, only if the kernel and a working persistent-storage backend supply them.
- Enabling this collector does not configure reserved memory, ramoops, or a guaranteed panic recorder. Missing pstore data is not proof that no panic occurred.

Download files you need to the host before rotation:

```bash
curl --fail http://audiox.local/api/rootfs/stdout.log -o stdout.log
curl --fail http://audiox.local/api/rootfs/stderr.log -o stderr.log
curl --fail http://audiox.local/api/rootfs/stdout.log.previous -o stdout.log.previous
curl --fail http://audiox.local/api/rootfs/stderr.log.previous -o stderr.log.previous
curl --fail http://audiox.local/api/rootfs/kernel.log -o kernel.log
curl --fail http://audiox.local/api/rootfs/kernel.log.previous -o kernel.log.previous
# Only if persistent crash records have been collected:
curl --fail http://audiox.local/api/rootfs/pstore.log -o pstore.log
```

These are separate from configuration backups. Kernel logging can still lose the last panic messages during a hard hang or power loss. For deeper investigation see [Troubleshooting](/guide/troubleshooting) and [serial diagnostics](/guide/hardware#serial-diagnostics).

## Firmware updates

### Prerequisites

1. Make and download a [configuration backup](/guide/config). Automatic boot rollback is not a substitute for one.
2. Record System's running version and board model.
3. Confirm reliable power, free storage, and a stable USB connection.
4. Choose the correct board's `.axu` from a trusted release. An SD `.img.xz` and a configuration `.axb` are not update archives.
5. Plan for audio interruption and potentially more than one reboot.

An AXU can replace the kernel, bootloader, boot configuration/command line, and runtime without formatting the persistent storage partition. It does not mean every file on storage is backed up or every configuration is compatible with a downgrade.

### Install a local AXU

1. Download the matching release archive and its SHA-256 sidecar.
2. Verify the file on the host before selecting it:

   ```bash
   sha256sum -c audiox-update-pi4.axu.sha256
   ```

   Use the Pi 5 filename for a Pi 5 archive. Stop on any mismatch.
3. In **System > Firmware Update**, choose the `.axu` under **Update Image**.
4. Click **Install Update** and confirm.
5. Keep the page and USB connection open through upload, validation, installation, and reboot waiting. Upload reaching 100% is not installation completion.
6. After reconnection, check System's version/kernel and any failure banner. Leave the new runtime healthy for at least 60 seconds so its trial boot can be confirmed.
7. Verify your basic audio route and saved settings before resuming critical use.

The current upload ceiling is 256 MiB. The server stores and validates the complete archive before replacing boot files, checks entry CRCs and metadata, rejects a different detected board for v2 archives, and requires the boot configuration's kernel name to match the archive. Checks are not cryptographic firmware signatures.

Older v1 archives lack the v2 board metadata and use `kernel8.img`; do not rely on them for board protection. Prefer current board-specific releases.

### Download and install from the browser

1. Keep the host computer connected to its normal internet network as well as AudioX USB.
2. Select **Stable** or **Nightly (may be unstable)**.
3. Click **Check GitHub Releases**.
4. Review the installed version, selected release, and board-matched archive.
5. Click **Download and Install Release**, review the confirmation (including possible downgrade), and continue.
6. Wait through download, upload, validation, reboot, and reconnection; verify the installed version afterwards.

Only the **browser/computer needs internet**. It queries GitHub without a token, identifies the board from System info (or a supported older model string), and chooses `audiox-update-pi4.axu` or `audiox-update-pi5.axu`. Already-installed releases disable the online install button.

Nightly lookup resolves the rolling release to a versioned nightly and compares it with the latest stable release by GitHub publication date: whichever was published later is selected, so a `1.14.2` published after `1.15.0-beta` wins. SemVer precedence only breaks ties when a date is missing or identical (a final stable version beats a prerelease of the same version; build metadata is ignored). If the nightly is missing or is not newer, it selects stable instead, including the stable download mirror entry. The browser uses the project's CORS-enabled download mirror for archive bytes and requires its tag/path/size to match GitHub metadata. It checks download size and AXU magic, then the device checks archive entries. **The browser does not independently verify the SHA-256 sidecar.** For a manual SHA-256 check, use the local-file workflow above.

If the mirror is unavailable, GitHub is rate-limited, or the browser is offline,
the status explains the failure. **Download matching AXU manually** remains an
alternative when a release asset exists.

### Update notifications

While the WebUI is open, the browser checks GitHub for the release channel selected in **System > Firmware Update** and shows a banner when an update is available. With no saved choice, the channel defaults to Nightly on nightly firmware and Stable otherwise. The choice is saved on the device (`update_channel=stable|nightly` in `config.txt`) and loaded through `GET /api/updates/channel`; changes use `POST /api/updates/channel` with `channel=stable|nightly`. It applies to every browser and survives restarts, firmware updates, and browser storage clearing until you change it (or restore/reset the device configuration), even when the Nightly channel offers a newer stable release. Old browser-only channel preferences are no longer used. Failed loads or saves show a warning. **View update** opens the Firmware Update card and runs a check; nothing is installed without confirmation. **Dismiss** hides the banner until a different release is published.

A release counts as an update when it was published on GitHub after the installed release. An installed version not found in the recent GitHub release history (for example a local or deleted build) is treated as older, so any published release is offered. Results are cached in the browser for 6 hours (1 hour after a failed check), or until the channel or installed version changes.

Clear **Notify me when an update is available** to stop the automatic checks. The setting is stored on the device (`update_notifications=0` in `config.txt`), so it applies to every browser; manual **Check GitHub Releases** still works.

### If an update appears to fail

- Do not immediately power-cycle or upload a second archive.
- Read the status, wait for reboot/USB enumeration, and try the raw USB IP if name resolution is the problem.
- A lost upload response can mean the device already started rebooting. Check its running version before retrying.
- Closing **Stop waiting** affects only the browser wait, not the device action.
- If the device returns to the previous version, preserve the automatic-revert report below.
- If it cannot reach its bootloader or never becomes reachable, recovery may require reflashing. Restore configuration from an external backup afterwards.

## Automatic update reversion

This protects update paths including complete AXU, runtime-only, and individual boot-file uploads, when the **currently installed runtime and bootloader already support recovery**.

1. Before changing boot files, the runtime saves a confirmed boot set in `/audiox/system/boot-backup/` and arms `boot-trial` with zero attempts. An existing backup is retained rather than overwritten by another unconfirmed update.
2. The bootloader counts subsequent trial boots. The watchdog reboots instead of merely restarting a failed/stalled runtime during the trial.
3. After the runtime stays healthy for 60 seconds, it removes trial state and the backup boot set.
4. If a boot starts with three attempts already used, the bootloader restores the saved boot files, writes `last-boot-failure.txt`, clears trial state, and reboots.

The report includes failed/restored versions, attempts, restored files, trial events, and runtime-log tails. It lives under `/audiox/system/`.

### Failure banner actions

- **View report** expands the report; **Hide report** collapses it.
- Copy or download the report before dismissing:

  ```bash
  curl --fail http://audiox.local/api/system/boot-failure -o last-boot-failure.txt
  ```

- **Dismiss** deletes the stored report and hides the banner. It does not perform a revert, reinstall the failed image, or clear the underlying cause.

If no backup was available, the banner can report a failed boot without claiming that restoration succeeded. `GET /api/system/boot-failure` returns `204` when no report exists.

There is no **Revert now** control. After a successful trial boot, the backup is
deleted; to return to an earlier version, install a compatible AXU or reflash.

::: danger Rollback is limited
It depends on boot reaching the bootloader and on failures causing a reboot. An early kernel/bootloader hang or loss of power while writing boot files can defeat it. The first update from firmware without recovery is not protected by a feature its old runtime does not have. Current generated command lines include `panic=10`; older cards may not. Never remove `rdinit=/init` from the single-line boot command line.
:::

## Boot stages and storage

AudioX uses a two-stage initramfs, not a persistent root Linux installation:

1. Raspberry Pi firmware reads the FAT boot partition, its `config.txt`/`cmdline.txt`, selected kernel, and both initramfs files.
2. `initramfs.cpio.gz` supplies the small bootloader `/init` and storage modules. `program.cpio.gz` supplies the runtime beneath `/program` in RAM.
3. The bootloader mounts storage as needed. If a staged runtime exists, it copies it to the boot partition and reboots into the new image.
4. It processes trial/recovery state, then chroots into `/program` and executes its `/init`.
5. Runtime `/init` is PID 1's watchdog. It supervises dnsmasq separately and launches `/sbin/audiox-main`.
6. The app loads early then normal modules, mounts persistent storage at `/audiox`, initializes logs/configuration, creates the USB gadget/network, and starts display, audio, MIDI, HTTP, and Bluetooth services.

| Location | Persistence and purpose |
| --- | --- |
| `/dev/mmcblk0p1`, FAT (`BOOT`) | Persistent kernel, firmware, device trees/overlays, boot config and initramfs files; bootloader uses `/mnt/boot`, update code mounts `/boot` |
| `/dev/mmcblk0p2`, ext4 (`STORAGE`) mounted at `/audiox` | Persistent user configuration, sounds, converter, logs, staging, and recovery state |
| `/audiox/config/config.txt` | Runtime user settings; distinct from boot partition `config.txt` |
| `/audiox/staging/program.cpio.gz` | Pending runtime for promotion on the next boot |
| `/etc`, `/sbin`, `/lib`, `/etc/www` in the running image | RAM-backed program image content; editing it is not a durable firmware customization |
| `/proc`, `/sys`, `/dev`, ConfigFS | Kernel/pseudo-filesystem state, not user backup contents |

Persistent does not mean included in a configuration backup. See [Config](/guide/config) for the precise managed files, restore semantics, and exclusions.

Outside a trial, the watchdog restarts an exiting or unresponsive runtime with capped backoff. Its startup grace is two minutes, then it expects periodic main-loop heartbeats (12-second timeout). Realtime audio does not supply that heartbeat. A runtime restart rotates logs and unbinds/rebinds the gadget, so the host may observe a USB disconnect even without a kernel reboot.
