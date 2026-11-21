# Desktop companion

The AudioX companion runs on your desktop and connects to the Pi over its USB
network. When connected, the WebUI shows a **Desktop** page with the host name,
operating system, and detected capabilities. The page disappears on disconnect.
Only one companion can be connected at a time.

This is an early version: automatic app routing, audio device controls, media
controls, shortcuts, and the system tray are not implemented yet. On Linux,
experimental manual stream routing is available in the Desktop page and via
`desktop.route`; see the [Lua API](/lua/api#desktop-stream-routing). On Linux, `pipewire` indicates a
detected PipeWire socket, and `default_sink` additionally requires `wpctl` on
the service's PATH. On Windows, `default_sink` indicates platform support.
Detection does not guarantee that the host audio service accepts a command.
The `route_stream` capability requires `pactl` on the companion's PATH.
On Linux, if `playerctl` is on PATH, the page also reports the aggregate MPRIS
playback status. Any playing player takes precedence over paused or stopped
players. No players and query errors are shown separately. Lua scripts can read
the status with `desktop.media_status()` and `desktop.is_playing()`.

## Connecting

The companion connects to `172.31.73.1:80` by default, without storing settings
on the host. It retries automatically if the USB link or Pi disappears. With a
KVM, the newly connected desktop keeps retrying until the previous connection
has closed or timed out. Capability detections are refreshed every five seconds;
the companion sends another hello only when the results change.

To try a different IPv4 address or port without saving settings, run the
executable with `--host <IPv4-address> --port <port>`. The Pi's HTTP service
normally listens only on its USB network address.

## Testing Linux stream routing

Start audio playback on the desktop, then run:

```sh
pactl list sink-inputs
pactl list short sinks
```

Find the desired application in the sink-input listing and use the number
after `Sink Input #` as the stream ID. In the Desktop page's **Move a Linux
audio stream** section, enter that ID and the first column of the destination
sink listing as the sink ID. Select **Move stream** and check the result.
This changes live playback; choose a stream and destination you are prepared to
move. IDs can change when applications or devices reconnect. If the section
doesn't appear, check that the companion is connected, PipeWire is running,
and `pactl` is on its PATH.

The companion attempts to restore its moves on disconnect or termination,
unless the stream has since been moved elsewhere. Restoration is best-effort,
not guaranteed after a crash, audio server restart, or device removal.
Manual routing rejects streams without a PulseAudio client identity because
the companion cannot safely identify them for restoration.
The page does not yet discover names or save automatic routing rules.

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

## Linux user service

The Linux release AppImage includes an optional systemd user unit. To install
it, run the following in an empty directory, substituting your downloaded
AppImage path:

```sh
mkdir -p "$HOME/.local/bin" "$HOME/.config/systemd/user"
install -m 755 /path/to/audiox-companion-linux-x86_64.AppImage "$HOME/.local/bin/audiox-companion.AppImage"
"$HOME/.local/bin/audiox-companion.AppImage" --appimage-extract usr/lib/systemd/user/audiox-companion.service
install -m 644 squashfs-root/usr/lib/systemd/user/audiox-companion.service "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
systemctl --user enable --now audiox-companion.service
```

The unit uses AppImage extraction at launch, so a FUSE mount is not required.
Use `systemctl --user status audiox-companion.service` and
`journalctl --user -u audiox-companion.service` to inspect its status and logs.
Stop and disable it with `systemctl --user disable --now audiox-companion.service`.

Windows releases provide an executable; an installer is not yet available.
macOS releases provide an unsigned Apple Silicon archive and have not yet been
tested by the project.

## Security

The companion uses the separate `audiox.desktop.v1` WebSocket protocol. Like
the WebUI, it does not authenticate native peers: use only on a trusted USB
network and do not expose it to the internet. See the [API reference](./reference#live-status-events)
for the hello and status-event formats.
