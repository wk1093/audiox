# Desktop companion

The AudioX companion runs on your desktop and connects to the Pi over its USB
network. When connected, the WebUI shows a **Desktop** page with the host name,
operating system, and detected capabilities. The page disappears on disconnect.
Only one companion can be connected at a time.

This is an early version: audio controls, routing, Lua host actions, and the
system tray are not implemented yet. Capability names describe host support,
not actions that AudioX can currently perform. On Linux, `pipewire` indicates a
detected PipeWire socket, and `default_sink` additionally requires `wpctl` on
the service's PATH. On Windows, `default_sink` indicates platform support.
Detection does not guarantee that the host audio service accepts a command.
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
