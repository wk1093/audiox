# AudioX Companion App

**Status:** The background companion connects and reports detected host
capabilities and Linux media playback status. Linux supports Pi-managed
automatic app routing, stereo-pair selection, and a system tray. Audio device
default selection is supported on Linux; media controls and shortcuts are not
yet implemented.

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

`pipewire` means a PipeWire socket was detected. On Linux, `default_sink`
requires `pactl` or both `wpctl` and `pw-dump` on PATH and allows the WebUI or
Lua to change the default playback device. If `pactl` cannot read the default,
the companion uses WirePlumber instead. It keeps the chosen backend for the
connection so a temporary failure cannot redirect changes to a different audio
server. Changes are sampled approximately once per second; rapid changes can
be missed. Windows and macOS do not currently support this.
Detection does not guarantee that the host audio service will accept a command.

On Linux, `route_stream` indicates that a PipeWire socket and `pactl` are
available. Use the Desktop page for automatic app rules or
`desktop.route(stream_id, sink_id)` for a manual Lua move; see the
[desktop guide](../docs/guide/desktop.md#automatic-linux-app-routing)
and [Lua API](../docs/lua/api.md#desktop-stream-routing) for limitations.
The companion attempts to restore its changes when it disconnects or stops.

On Linux, `pipewire_nodes` indicates that `pw-dump` is available and the Pi can
request a snapshot of PipeWire nodes. The Desktop page displays the discovered
node and application names. PipeWire node IDs are not PulseAudio stream IDs.
`auto_route` requires `pactl`, `pw-dump`, `pw-loopback`, and `pw-metadata`.
With the same tools, `forward` means the companion can run Pi-stored
forward rules. Each rule keeps a `pw-loopback` from one stereo pair of a capture
device to a playback device; see the
[desktop guide](../docs/guide/desktop.md#forwarding-a-device-to-desktop-speakers).

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
  A C++ compiler with static libraries, Go 1.22 or newer, and appimagetool are required to build.
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

For local Linux testing, `make desktop-update` builds the current AppImage,
installs it for the current user, and restarts (or starts) the systemd user
service. It does not enable the service at login. When the bundled WirePlumber
policy changes, the restarted companion may briefly interrupt audio to activate it.

## Linux installation and updates

Run the release AppImage to install it for your user. It stores copies under
`$XDG_DATA_HOME/audiox/appimages` and installs the launcher
`$XDG_DATA_HOME/audiox/audiox-desktop` (default: `~/.local/share/audiox`).
The launcher uses the currently selected AppImage, including after updates.
The installed systemd user unit is not enabled without your confirmation; the
first launch opens a browser page where you can choose to enable it at login.
The tray's **Login service** menu opens the same page. A running companion stays
running when login startup is disabled. A customized existing unit is preserved.

An online release update started in the Pi's WebUI also asks a connected Linux
AppImage companion to install the same release. If the desktop update fails or
the companion is unavailable, the Pi update continues with a warning. Manual
firmware uploads do not update the companion.

The tray uses StatusNotifier and the bundled AudioX logo. Right-click it and
select **Update desktop and Pi** to open a local browser page. Select stable or
nightly and confirm: the companion requires a connected Pi, downloads both
artifacts from the same release, verifies their SHA-256 checksums, and uploads
the board-matched firmware. The Pi reboots and the companion restarts. An update
can replace a newer installed version. Browser and systemd user services are
needed for those respective features; errors are reported in companion logs.

Use `systemctl --user status audiox-companion.service` to inspect the service,
or `systemctl --user disable --now audiox-companion.service` to stop it.

## WirePlumber routing policy

The Linux AppImage automatically installs and updates its WirePlumber 0.5
policy in the user's XDG data and configuration directories. On installation
or update it restarts the active WirePlumber user service, briefly interrupting
audio. If systemd user services are unavailable, it reports the failure and
the policy loads the next time WirePlumber starts.

The policy routes matching new streams to the companion's
virtual stereo channels before WirePlumber links them. It also protects AudioX's
internal remap streams from redirection by applications such as EasyEffects.
Without the policy, application-selected virtual channels remain usable, but
automatic AudioX rules report an error. Removing `audiox-routing.lua` from
`$XDG_DATA_HOME/wireplumber/scripts` and `90-audiox-routing.conf` from
`$XDG_CONFIG_HOME/wireplumber/wireplumber.conf.d`, then restarting WirePlumber,
removes the policy. The AppImage reinstalls it when next started. If the XDG
variables are unset, they default to `~/.local/share` and `~/.config`.

Features depend on the host OS. The versioned handshake reports the host OS and
detected capabilities; controls will be added in future versions.

Desktop settings are managed and stored on the Pi; the Linux AppImage installs
the routing policy on the host.

Once connected, the Pi's web UI has a page for configuring desktop behavior.

## Linux Features
- Mananging pipewire routes from lua scripts
- Changing system default device from lua scripts
- separate automatic app routing (using fake sinks). Allows setups like routing spotfiy seperately from other apps (by increasing the number of USB gadget audio input channels from 2 to 4, and using 3/4 for spotify).
- Forwarding a stereo pair from a capture device (such as AudioX channels 3/4) to a desktop playback device such as HDMI speakers.

## Windows Features
- Changing system default device from lua scripts
- Separate automatic app routing (THIS WILL BE WAY LATER). This will require me to implement a custom windows driver, since a single audio device cannot be routed separately with 4 channels acting as 2 stereo pairs. I will need to create virtual audio devices to achieve this functionality.

## General Features
- Initial handshake with the Pi to negotiate version compatibility and broadcast host OS.
- Automatic detection and management of audio devices connected to the system.
- Desktop settings are managed and stored on the Pi.
- Desktop routing configuration is done through the Pi's web UI.