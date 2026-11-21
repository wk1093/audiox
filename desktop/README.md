# AudioX Companion App

**Status:** The background companion connects and reports detected host
capabilities and Linux media playback status. Experimental Linux stream routing
is available through Lua. Automatic app routing, audio device controls, media
controls, shortcuts, and the system tray are not yet implemented.

## Connection

The companion connects to `172.31.73.1:80` over USB and retries automatically
when the device or USB connection disappears. This also handles KVM switches:
only one desktop can be connected at a time; the next one retries until the old
connection is gone. It checks for capability changes every five seconds and
sends an updated hello only when something changes.

For testing or a different USB address, start it with
`audiox-companion --host <IPv4-address> --port <port>`. No connection settings
are saved by the companion. On connection, the WebUI shows a **Desktop** page
with the hostname, OS, and detected capability strings; the page disappears
when disconnected.

`pipewire` means a PipeWire socket was detected. On Linux, `default_sink` means
that socket and an executable `wpctl` were detected; on Windows it denotes
platform support. These are host detections, **not** implemented AudioX actions.
Detection does not guarantee that the host audio service will accept a command.

On Linux, `route_stream` indicates that a PipeWire socket and `pactl` are
available. Use the Desktop page or `desktop.route(stream_id, sink_id)` to move
a stream; see the [desktop guide](../docs/guide/desktop.md#testing-linux-stream-routing)
and [Lua API](../docs/lua/api.md#desktop-stream-routing) for limitations.
The companion attempts to restore its changes when it disconnects or stops.

On Linux, `mpris` means `playerctl` is available on PATH. The companion queries
all MPRIS players every five seconds; the WebUI shows Playing if any player is
playing, otherwise Paused if any is paused, otherwise Stopped. It reports no
players or a query error separately. Lua scripts can read this status using
`desktop.media_status()` and `desktop.is_playing()`.

The native client uses `/ws/desktop` with the `audiox.desktop.v1` WebSocket
subprotocol. The protocol is unauthenticated, like the existing USB WebUI;
use it only on a trusted connection and do not expose it to the internet.

## Building

From the repository root, use the native toolchain for the target platform:

- Linux: `make desktop-linux APPIMAGETOOL=/path/to/appimagetool` produces
  `out/desktop/audiox-companion-linux-x86_64.AppImage` on x86-64.
  A C++ compiler with static libraries and appimagetool are required to build.
  The executable and AppImage runtime are statically linked; running normally
  needs a working FUSE mount. Without FUSE, pass `--appimage-extract-and-run`.
- Windows x86-64: install MSYS2 with `make` and `mingw-w64-x86_64-gcc`,
  open a MINGW64 shell, and run `make desktop-windows CXX=g++`. The output is
  `out/desktop/audiox-companion-windows-x86_64.exe`; an installer is TODO.
- macOS: `make desktop-macos` produces an unsigned
  `out/desktop/audiox-companion-macos-<architecture>.tar.gz` containing the
  executable. Apple's command-line developer tools are required.

`make desktop-clean` removes these outputs. Builds are native, not cross-compiled.
CI publishes Linux x86-64, Windows x86-64, and macOS Apple Silicon builds with
stable and nightly releases. The macOS build has not yet been tested by the project.

## Linux user service

The AppImage includes an optional systemd user unit. It starts the AppImage
without requiring a FUSE mount and restarts it after failures.

To install the unit from a release AppImage, run these commands in an empty
directory, replacing the source path with your downloaded AppImage:

```sh
mkdir -p "$HOME/.local/bin" "$HOME/.config/systemd/user"
install -m 755 /path/to/audiox-companion-linux-x86_64.AppImage "$HOME/.local/bin/audiox-companion.AppImage"
"$HOME/.local/bin/audiox-companion.AppImage" --appimage-extract usr/lib/systemd/user/audiox-companion.service
install -m 644 squashfs-root/usr/lib/systemd/user/audiox-companion.service "$HOME/.config/systemd/user/"
systemctl --user daemon-reload
```

Enable it at login with
`systemctl --user enable --now audiox-companion.service`. Use
`systemctl --user status audiox-companion.service` to check it, and
`systemctl --user disable --now audiox-companion.service` to stop it.

The planned desktop system tray will allow AudioX to interact with the desktop,
including audio routing and settings from Lua scripts running on the Pi.

Features depend on the host OS. The versioned handshake reports the host OS and
detected capabilities; controls will be added in future versions.

The desktop app has NO configuration or settings stored on the host, all options are managed and stored on the pi.

The desktop app has no GUI element, once connected to the pi, the web ui on the pi will have a new page for configuring desktop behavior.

## Linux Features
- Mananging pipewire routes from lua scripts
- Changing system default device from lua scripts
- separate automatic app routing (using fake sinks). Allows setups like routing spotfiy seperately from other apps (by increasing the number of USB gadget audio input channels from 2 to 4, and using 3/4 for spotify).

## Windows Features
- Changing system default device from lua scripts
- Separate automatic app routing (THIS WILL BE WAY LATER). This will require me to implement a custom windows driver, since a single audio device cannot be routed separately with 4 channels acting as 2 stereo pairs. I will need to create virtual audio devices to achieve this functionality.

## General Features
- Initial handshake with the Pi to negotiate version compatibility and broadcast host OS.
- Automatic detection and management of audio devices connected to the system.
- No configuration or settings stored on the host; all options are managed and stored on the Pi.
- No GUI element on the desktop; all configuration is done through the Pi's web UI.