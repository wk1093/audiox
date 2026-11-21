# Desktop companion

The AudioX companion runs on your desktop and connects to the Pi over its USB
network. When connected, the WebUI shows a **Desktop** page with the host name,
operating system, and detected capabilities. The page disappears on disconnect.
Only one companion can be connected at a time.

This is an early version: audio device controls, media controls, and shortcuts
are not implemented yet. On Linux, the Desktop page supports
automatic playback and capture routing without scripts. On Linux, `pipewire` indicates a
detected PipeWire socket, and `default_sink` additionally requires `wpctl` on
the service's PATH. On Windows, `default_sink` indicates platform support.
Detection does not guarantee that the host audio service accepts a command.
The `route_stream` capability requires `pactl` on the companion's PATH, and
`auto_route` also requires `pw-dump`, `pw-loopback`, and `pw-metadata`.
On Linux, if `playerctl` is on PATH, the page also reports the aggregate MPRIS
playback status. Any playing player takes precedence over paused or stopped
players. No players and query errors are shown separately. Lua scripts can read
the status with `desktop.media_status()` and `desktop.is_playing()`.

When `pw-dump` is available on Linux, the Desktop page also lists PipeWire
nodes, including active application streams. The dropdowns refresh automatically
and can be refreshed manually.

## Connecting

The companion connects to `172.31.73.1:80` by default, without storing settings
on the host. It retries automatically if the USB link or Pi disappears. With a
KVM, the newly connected desktop keeps retrying until the previous connection
has closed or timed out. Capability detections are refreshed every five seconds;
the companion sends another hello only when the results change.

To try a different IPv4 address or port without saving settings, run the
executable with `--host <IPv4-address> --port <port>`. The Pi's HTTP service
normally listens only on its USB network address.

## Automatic Linux app routing

Start playback or capture in the application, open **Desktop**, and select the
direction, application, destination, and stereo pair. Select **Add rule**.
For a four-channel USB device, channels 3/4 select its second stereo pair.
The application must have an active audio stream when you create the rule;
afterwards the companion reapplies it when matching streams appear.

With **Enable AudioX virtual stereo channels** on (the default), each stereo
pair on an AudioX USB device is also exposed as a separate desktop device,
such as **AudioX Channel 1** and **AudioX Channel 2**. Select these directly
in applications or in EasyEffects. The setting is stored on the Pi. Virtual
devices exist while the companion is connected; disabling them or disconnecting
the companion removes them, and applications may fall back to another device.

Rules are stored on the Pi and included in configuration backups. They apply
to any connected Linux desktop with matching names. The first matching rule
wins. Exact names are case-sensitive; the optional advanced patterns are POSIX
extended regular expressions and match substrings. A destination pattern must
match exactly one device. At most 16 rules and 32 active automatic moves are
supported. A destination must expose an even channel map; unavailable devices,
invalid pairs, and routing failures are reported in the page and companion logs.

The companion uses PipeWire loopbacks for AudioX stereo pairs and PulseAudio
remap modules for other destinations. The Linux AppImage automatically installs
the WirePlumber 0.5 routing policy to apply AudioX app rules before initial
linking; it also protects internal remap streams from being redirected by
other applications. Installation or upgrades restart the active WirePlumber
user service and briefly interrupt audio.
See the [companion README](https://github.com/wk1093/audiox/blob/main/desktop/README.md#wireplumber-routing-policy).
It attempts to restore streams when a rule is removed or the connection closes,
without overwriting a stream that was moved elsewhere. Restoration is best-effort
and cannot be guaranteed after a crash, audio-server restart, or device removal.
Manual Lua moves cannot override streams currently managed by an automatic rule.

## Lua stream routing

For custom automation, scripts can use the same IDs with
`desktop.route(stream_id, sink_id)`. For example, replace the numbers below
with current IDs and enable device-management permission when saving:

```lua
hook("run", function()
  desktop.route(11, 2)
end)
```

The script queues the move asynchronously; check device logs for failures.
For a read-only test of the media connection, run this script and inspect the
device logs:

```lua
hook("run", function()
  log("Desktop media: " .. desktop.media_status())
end)
```

## Linux installation and tray

Running the release AppImage installs a user-local copy, launcher, and systemd
user unit. On first launch, choose whether to enable the unit at login in the
browser page; it is not enabled automatically. The launcher uses AppImage
extraction, so a FUSE mount is not required.

Right-click the AudioX tray icon to open the Pi, configure login startup, or
update the desktop and connected Pi together. The update page lets you select
stable or nightly and confirm the Pi reboot; it verifies both downloads before
uploading firmware. See the [companion README](https://github.com/wk1093/audiox/blob/main/desktop/README.md#linux-installation-and-updates).
Use `systemctl --user status audiox-companion.service` and
`journalctl --user -u audiox-companion.service` to inspect its status and logs.

Windows releases provide an executable; an installer is not yet available.
macOS releases provide an unsigned Apple Silicon archive and have not yet been
tested by the project.

## Security

The companion uses the separate `audiox.desktop.v1` WebSocket protocol. Like
the WebUI, it does not authenticate native peers: use only on a trusted USB
network and do not expose it to the internet. See the [API reference](./reference#live-status-events)
for the hello and status-event formats.
