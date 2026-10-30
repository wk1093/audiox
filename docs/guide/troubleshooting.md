# Troubleshooting

Start with one source and one output. Remove optional effects and automation
from the test until that path works. Keep output levels low when changing routes
or gain; a wrong connection can produce unexpectedly loud audio.

## The WebUI does not open

1. Confirm power and a completed boot.
2. Check that the host cable carries data and reaches the Pi's gadget-capable
   port, not merely a power input or a USB host port.
3. Wait for the host's USB network interface to appear.
4. Try the numeric device address from [networking](./networking), instead of
   relying only on `audiox.local`.
5. Check browser proxy/VPN settings and whether another network uses the same
   subnet.
6. If it stopped during **Reload Config**, reconnect the USB interface and retry.
   If it stopped during an update or restore, allow the reboot sequence to finish.

A temporary connection warning does not prove that the Pi rebooted. Avoid
reflashing merely because the hostname failed to resolve.

## The page loads but the routing canvas does not

The interface references external fonts and LiteGraph assets. A host without
internet access, a restrictive content blocker, or an unavailable CDN may
prevent graph functionality even though the device serves HTML.

Check the browser's developer-console/network errors, then retry with a browser
that can load those assets. This documentation site is built separately and
does not provide an offline replacement for missing WebUI scripts.

## No sound

Follow the signal one step at a time:

1. Does the source itself produce audio? For a soundboard test, use a short,
   successfully converted clip.
2. Does the intended source appear in **Routing**?
3. Is every link present, including both sides of any inserted effect?
4. Did you choose **Save Routing**?
5. Are the source and intermediate-node volumes nonzero? Gains apply to outgoing
   edges; a final sink's gain is not a master attenuator for its incoming audio.
6. Are the correct destination channels connected, and has the host selected
   AudioX as its recording/playback device?
7. Does an effect have inappropriate gain, gate, pitch, or cut settings?
8. Does the physical output work outside that route?

The routing canvas does not currently have live meters. If a local display is
available, use its device/channel meters to check for activity, alongside host
application meters and logs. Output activity with silence at the speaker points
toward the destination, host selection, or physical path; no source activity
suggests checking the input device first.

Use the [routing](./routing), [effects](./effects), and [volume](./volume) guides
for their specific controls and restrictions.

## Distortion, pops, or unstable audio

- Reduce gains and simplify the graph. Multiple sources can sum and clip.
- Disconnect unused devices or unnecessary Bluetooth paths while isolating the
  issue. Bluetooth has buffering/latency characteristics unlike a wired path.
- Check CPU/load information and logs for PCM/device errors.
- Confirm stable power and compatible audio hardware.
- If changing USB channel count/sample size, apply a known-working format and
  reopen the device in host software.
- Remember that a build's sample rate and buffer size affect CPU/latency;
  changing them is a developer task, not a universal performance fix.

Do not assume more threads will fix an overloaded signal chain. Audio
processing already uses a dedicated thread, and device maintenance is separate
from per-sample processing.

## Soundboard problems

| Problem | Likely explanation |
| --- | --- |
| Clip is cut short | Current loaded-clip cap is normally eight seconds |
| More files exist than loaded pads | Cache/list capacity is 64; inspect reload status and logs |
| File upload succeeds but no audio plays | Conversion/load problem, unsupported source, missing route, or insufficient memory |
| Pad selects instead of plays | Sampler selection mode is active |
| Saved order conflicts | Another client changed the library; refresh and reorder again |
| Removed file is still referenced in Lua | Macro source is not rewritten automatically |

See [Soundboard](./soundboard).

## MIDI, lights, and macros

- A sound or action bound to a note is generally not channel-isolated; macros
  and the sampler keyboard use explicit zero-based channels.
- Reserve distinct notes for global actions, effect bypass, sound pads, and
  automations unless combined behavior is intentional.
- No LED response can mean a wrong output channel, an unsupported controller
  lighting protocol, or **Dim lights** being active.
- Missing host MIDI can mean USB passthrough is disabled, not missing controller
  input. Passthrough is controller-to-host only.
- A macro's Run button queues the saved definition. Reload the list to see
  runtime errors.
- Keyboard actions require enabled USB Keyboard and a connected host.
  `light()` also requires a MIDI binding, and `midi.*` needs the controller.
- Lua is bounded and intentionally has no filesystem, shell, or network access.

See [MIDI](./midi), [Macros](./macros), and
[Lua troubleshooting](/lua/limits-and-troubleshooting).

## A change vanished after restart

Check whether it was meant to persist:

- Drawn routes need Save Routing.
- USB form changes need Save Config, then Reload Config to apply.
- Lua state and script-driven effect/volume changes are runtime-only.
- Dim lights and the active sampler selection are runtime state.
- A graph's browser layout is not its device signal-path configuration.

Download a fresh [backup](./config#back-up-your-setup) after saving a working setup.

## Backup, restore, or update rejected

Check file type before anything else: `.axb` is a setup backup, `.axu` is a
firmware update, and `.img.xz` is an SD-card image. Use the correct page/tool.

Verify the board matches the update and there is sufficient free storage for
staging/extraction. A checksum/validation failure is a reason to redownload or
investigate the file, not to bypass verification. Keep stable power throughout.
For a lost upload connection, inspect the device before retrying: staging may
already have finished.

An automatic boot-revert banner is diagnostic information. Read and save the
report before dismissing it. Rollback is not a substitute for backups or a
guarantee against SD-card or boot-partition failures.

## Gather a useful issue report

Include:

- Pi model, AudioX version/build channel, and kernel from **System**.
- Connected audio/MIDI/display hardware and how it is connected.
- Whether a minimal source-to-output route reproduces the problem.
- Exact steps, expected behavior, actual behavior, and relevant log excerpts.
- Whether the problem began after a USB reload, restore, or firmware update.

Do not publicly attach a complete configuration backup: it includes Bluetooth
pairing keys and may include private audio or macro text. Review logs and
screenshots before sharing them.
