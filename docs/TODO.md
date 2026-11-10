# TODO

Roadmap for future audiox development.

## v1.0.0 - Complete ✓

- [x] Real-time audio engine with ALSA capture/playback
- [x] USB UAC2 gadget (bidirectional audio)
- [x] Framebuffer UI with touch + MIDI input + level metering
- [x] HTTP API (config, routing, file upload, sync/restart/shutdown)
- [x] MIDI note-to-SFX mapping
- [x] Soundboard with file-based clip slots
- [x] USB microphone input support
- [x] Gadget audio I/O with proper gain control
- [x] CPU/RAM metrics display on fbui

## Next Steps - v1.1+

- [x] Improve soundboard UI to be more usable and less confusing
- [x] Polish UI:
  - [x] switch to using LiteGraph for the routing.
  - [x] Make it so "things" are reloaded periodically so that new devices show up on the WebUI

## Later - v1.2+

- [x] Random thing: Resize device containers on fbui depending on # of channels so channel levels don't clip out of the device box.
- [x] Make soundboard more flexible
  - [x] Make soundboard able to output midi as well to display lights on a connected controller
  - [x] Make soundboard have two play modes: "play" and "hold" (where "hold" mode will play the clip as long as the button is held down, and "play" mode will play the clip once and then stop)
  - [x] Add the ability to map a button to a "stop all" function that will stop all currently playing clips


## More Later - v1.3+

- [x] Polyphonic soundboard playback
- [x] Ability to set a certain midi channel as "keyboard" so that those notes don't map to soundboard or other triggers, but are used for a sampler instrument.
- [x] Ability to set a certain midi button to be a "sampler mode" toggle, where soundboard buttons don't trigger clips, but set the current sample to be played by the keyboard notes.

The above last two features are mainly to improve my setup, but could probably be useful for others as well. This is kinda tailored to my midi controller (AKAI APC Key 25) which has an upper section with a bunch of buttons on one channel (which I will use for soundboard clips, and control buttons for sampler mode, muting, effect, etc), and also has knobs which i can use to control effects and volumes. the lower section is a keyboard on a separate midi channel, which I would like to use as a sampler keyboard, so I can select a soundboard clip and play it at different pitches. If sample mode is enabled, and we select clip X for example, and then we disable sampler mode, than the soundboard buttons will trigger clips again, but the keyboard will still play the last selected clip X at different pitches.

## Even More Later - v1.4+

- [x] Volume sliders for all devices, with proper gain control for USB gadget input
- [x] Add a static ffmpeg build to the initramfs so that uploaded audio files can be converted to wav (which I can easily parse and play)

## Very Later - v1.5+

- [x] Add audio effects (reverb, delay, etc) to the audio engine, and make it so they can be routed to any output, and have their parameters controlled via the HTTP API as well as bindable MIDI CCs and buttons (for toggling effects on/off)
  - [x] Phase Vocoder based pitch shifting
  - [x] Reverb
  - [x] Basic Noise cancellation (probably just a gate for now, more advanced later)
  - [x] Gain
  - [x] Distortion
- [x] Effect midi integration
  - [x] Allow controlling effects with MIDI CCs and buttons
  - [x] Midi lights for the effect enable/disable buttons
- [x] change routing config to identify devices differently — now uses the ALSA card ID string (e.g. `t6`, `uac2gadget`) instead of card numbers. Card IDs are stable across reboots for a given device model.

## Bugfixes and stabilization v1.6+
- [x] Apply the audio stability fixes and suggestions
  - [x] Use direct ALSA MMAP transfers where supported, with RW-interleaved fallback. Thanks u/Brer1Rabbit
  - [x] mlockall before starting the audio engine to prevent page faults from causing xruns. Thanks u/Kamran-nottakenone
- [x] Prefer USB vendor/product identity plus a serial hash for stable audio device IDs; use the USB port path when no serial is exposed.
- [x] Investigate audio-engine latency changes. The processing block is 128 frames, but PCM setup tries 512- and 256-frame periods before 128; defer tuning until Pi-side latency/xrun measurements are available.
  

## Pre-Pre-Release - v1.7+

- [x] Bluetooth support: controlled by web ui, open a specific page on the webui, and it will start listing out bluetooth devices, and you can click one to pair, and it appears in the graph as a device. Will have a specific device naming, and it's graph elements will be persistent even when it isn't connected, so that when it does connect, it remembers how it was routed the last time it was used. I also need to implement some sort of routing tool like a blank effect, so that I can route to a "fake output" that routes to both wired and bluetooth so that both always work, and we have an in between instead of a bunch of individual connections to both. I guess we can use a gain module set to 1, but that adds extra processing, this blank module should be purely for nice routing in the ui and routing config, but in the audio processing should be compiled out.
- [x] fbui soundboard, for people without a midi controller, set a config option in the webui, and the normal fbui will be replaced with a steamdeck-like soundboard interface (with optional multi-page via tabs and/or swipe)
- [x] Investigate occasional "bad boots" where there is constant underruns in the logs, but a reboot always fixes it.

## Big Refactor and fixes - v1.8+
- [x] Fix versioning system (use 1.8.0-beta and whatnot instead of putting 1.8 dev stuff under 1.7.x), implement an extra version field everywhere (makefile, C++ source, JS source, etc)
- [x] Make a config TUI like the linux kernel makefile has to allow configuring for different systems, environments, and be able to change the currently #define static settings like the buffer size and sample rate.
- [x] Split up code better so that we can partially compile the audio subsystem (the audio graph should be a bit more abstract, and shouldn't know about how devices actually work) so that we can test effects and stuff on my desktop instead of re-uploading to the Pi.
  - [x] Keep one direct realtime processing path after source/sink callbacks caused device-side underruns; exercise that same runtime code in host tests.
  - [x] Extract the fixed-capacity PCM capture ring and SRC cursor state from the ALSA stream object; keep ALSA acquisition in the Pi adapter.
  - [x] Move ALSA capture stream ownership, device attachment/remapping, source gadget identity, and fixed PCM/SRC state out of RuntimeGraph into an explicit capture backend; keep ALSA I/O on the direct Pi path.
  - [x] Cover route mixing, node processing, and effect execution in direct-runtime host tests without a separate desktop engine.
- [x] "Cut" effect/feature to mute or unmute a route, with configurable matching input/output channel counts and MIDI/Web UI toggling.
- [x] Run the application as a child of a small PID 1 watchdog; restart it after exit or a stale main-loop heartbeat.

## Pre-release - v1.9+
- [x] Smarter bootloader that can detect bad initramfs and boot into a backup one:
  - [x] File for storing the number of bad boots, and a feature in the main initramfs that will reset that counter once everything has booted properly. If the bootloader sees that this counter is above 2 or 3, it will boot into a backup initramfs instead of the main one.
  - [x] Make it so that the bootloader can update the kernel as well.
  - [x] Is it possible to add a custom listener or hook into a kernel panic? Maybe automatically make a kernel panic trigger a reboot into the backup initramfs? (`panic=10` reboots, which counts as a failed trial boot)
- [x] Somehow be able to update the bootloader itself. This could be done by having the main initram update the bootloader, since the bootloader updates the main initram, but they can't edit themselves. So maybe just update the bootloader first from the main initramfs right when the update is triggered, and then reboot into the new bootloader to update the main initramfs. Or I could make them two separate processes, where you could choose to update one or the other. That is probably better since the bootloader won't be updated as often, and we won't have to require a larger update file. The update file will eventually be a custom format that is kinda like a tar, it contains a bit of metadata, and then two separate files (or just one). The update metadata will contain a version number, and then it will also tell the program if it is a bootloader update, initram update, or both.
- [x] Improve make config options, including board selection and hardware-tested Pi 4 and Pi 5 defaults.

## USB audio fixes and fault diagnostics - v1.10
- [x] Correct boosted/distorted USB audio in both directions, including Windows; verify neutral signal levels without hidden gain.
  - [x] Remove the fixed 10x gadget-input boost and avoid reapplying gain to capture-underrun history.
  - [x] Test USB audio in both directions on Windows and Linux.
- [x] Honor host USB volume and mute independently of AudioX Web/MIDI gains; verify Windows and Linux after reconnect and gadget reload.
  - [x] Configure non-boosting UAC2 controls and apply cached host attenuation/mute at both USB boundaries; add host regression tests.
- [x] Implement static build of dnsmasq for DHCP and DNS management, to make plug-and-play work.
  - [x] Pinned, checksummed static aarch64 dnsmasq build (`tools/build/build_dnsmasq.sh`), staged at `/sbin/dnsmasq` with `root/etc/dnsmasq.conf`.
  - [x] Device address moved to `172.31.73.1/24` (DHCP clients reject `169.254/16` leases); DHCP pool without a router, DNS for `audiox`/`audiox.lan`.
  - [x] PID 1 watchdog supervises dnsmasq in its own process group with restart backoff; host tests and a namespace DHCP/DNS check pass.
  - [x] Built-in mDNS responder for `audiox.local` (host DNS such as Tailscale or a router `lan` domain can shadow the device's DNS); packet tests and a namespace check pass.
  - [x] Hardware check: Windows and Linux hosts get a lease and open `http://audiox.local/`, including after replug, gadget reload, and runtime restart.
- [x] TESTED: Bugs left
  - [x] Windows works mostly now, but device still shows up as "Source/Sink" instead of "AudioX"
    - [x] Set the UAC2 `function_name` (kernel default "Source/Sink") and terminal/volume strings to `AudioX ...` (`AUDIOX_USB_AUDIO_NAME`).
    - [x] Verified the host receives the AudioX strings (lsusb iFunction/iProduct/iInterface), but Windows still showed "Speakers (Source/Sink)" because it caches endpoint names per USB serial, even across a Device Manager uninstall.
    - [x] USB serial is now `AUDIOX-<board serial>` (was the fixed `AUDIOX0001`), so Windows enumerates fresh devices and two boards no longer clash on one host.
    - [x] Verify on Windows
  - [x] Windows also works with the DNS sometimes, but if tailscale is used, it won't (and probably if a home router uses .local). Maybe I should consider not using "audiox.local" and instead do some weird DNS on my actual real domain to somehow make it route to a local server, so someone could do "audiox.wklab.net" and it would route to THEIR local server, if they didn't have an audiox machine, it would just fail. Or maybe I could just fake it and have the mDNS responder respond with "audiox.wklab.net"? IDK if mDNS allows using a real domain instead of .local though...
    - Decision: keep `http://audiox.local/` as the primary URL and `http://172.31.73.1/` as the always-works fallback. Tailscale "Use Tailscale DNS" on Windows sends every name (including `.local`) to 100.100.100.100 via NRPT (tailscale/tailscale#15093), so no device-side name can bypass it except a public DNS record.
    - [x] Backup URL `http://audiox.wklab.net/` (public `A` record to `172.31.73.1`, also in the device's dnsmasq `host-record`); README documents the plain-HTTP browser warning. No shared TLS certificate: its private key would ship in every image and get revoked.
    - Hosts only send mDNS for `.local`, so the responder cannot serve `audiox.wklab.net`. The public record above is the workaround.
  - [x] Convert the SVG icon text to path so that it doesn't rely on external fonts.

## Features and quality of life - v1.11
- [x] Improve webui, especially errors, don't show nasty JS stack traces, or html responses from api (json data) to the user.
  - Styled notifications and confirmation/loading dialogs, readable API errors, automatic reconnect/restart detection, and clearer Config/System layout.
- [x] Improve display detection: check framebuffer/DRM connector health every 500 ms, release display and touch resources while disconnected, and reinitialize on reconnect or mode changes without restarting audiox. Non-DRM cable detection depends on the framebuffer driver exposing device/layout changes.
- [x] Optional USB MIDI passthrough with WebUI off/unmapped/all modes, 500 ms discovery, bounded nonblocking output, and complete MIDI function removal when disabled.
- [x] Optional USB keyboard chords: channel/note mappings, WebUI editor, automatic release, bounded trigger queue, and complete HID function removal when disabled.
- [x] Macro WebUI: learn a MIDI pad, record physical keyboard chords with modifiers, edit saved mappings, and configure per-macro idle/pressed lighting.
- [x] Standalone Macros page: names, optional MIDI binding/web-only macros, Run, and stable-ID edit/remove; preserve chord recording, MIDI learn and lighting.
- [x] Document named scripts, web/MIDI events, per-action device requirements, and runtime-only effect/volume controls.
- [x] Restricted Lua macro sequences: bounded cooperative key taps, holds/releases, delays and pad lighting; no standard libraries or background interpreter.
- [x] Lua text typing with adjustable intervals (US-layout ASCII), live note-on event values, and bounded per-macro state. Unicode/other host layouts are not supported; held keys automatically release on completion/cancellation.
- [x] Expand safe Lua utilities: trigger-time clocks, formatting/conversion, string/table/math helpers, random choices, queued logging, soundboard playback and controller MIDI messages.
- [x] Improve documentation for the project, including OS setup guides and usage examples
  - [x] Set up VitePress at wklab.net/audiox/docs with Pages deployment and descriptive Lua guides, reference and cookbook.
  - [x] Write OS setup, hardware/networking, every WebUI page, audio routing, Bluetooth, display, backups, updates/recovery, troubleshooting and development guides.

## Last few improvements - v1.12
- [x] Add per-device Bluetooth auto-connect, saved-device reconnection retries, and pause idle A2DP streams so multipoint headsets can switch sources.
- [x] Verify Bluetooth reconnection, multipoint handoff, and audio resume behavior with physical devices; continue improving connection reliability.
- [x] Fix support for Bluetooth phone connection.
- [x] Investigate using LV2 plugins for effects, and if possible, make it so that the user can upload their own LV2 plugins to the device and use them in the audio engine. This would allow for a lot more flexibility and customization for users who want to use their own effects.
  - LV2 is not a good fit for this project (for now), but I will make a custom plugin system tailored to the audio engine's requirements. It will be a simple dynamic library (along with some metadata, packed into a single custom file format), loaded at runtime by the audio engine.
- [x] Generate release notes from `[feat]` and `[fix]` commit markers, including multiple entries in one commit; nightly notes compare to the latest nightly or stable, and stable notes include all changes since the previous stable release.

## Pi 5 Testing - v1.12.x
- [x] Test Pi 5 hardware for compatibility and performance with the current audio engine and WebUI.
  - [x] Fix configuration backup restore failures.
  - [x] Expand image storage to fill the SD card.

## Effect, Soundboard, and Engine Improvements - v1.13
- [x] Soundboard modes: Normal mode retains current behavior; grid mode supports configurable size, MIDI corner learning, explicit sound placement, blank cells, and conversion of existing MIDI mappings without remapping. The framebuffer and WebUI both display the configured grid.
- [x] Improve soundboard edit UI
- [x] Expand Lua with bounded configuration reads and atomic queued writes, save-time permission warnings, and MIDI release/CC, sound voice lifecycle and selected MIDI controller connection hooks. Configuration access uses a six-file allowlist; pairing credentials remain inaccessible.
- [x] Add multi-hook Lua callback scripts with explicit state-only persistence and code-derived subscriptions; expand source, memory, instruction, action and typing budgets while retaining sandbox and audio-thread isolation.
- [x] Rename automation to Scripts, remove sequence/shortcut editor modes, and automatically convert existing saved entries to callback source without changing their bindings or permissions.
- [x] Improve effect system to be more user friendly
  - [x] Provide readable defaults and editable saved names in Effects, Routing, Volume, and notifications; retain stable IDs in advanced details for scripts.
  - [x] Improve parameter UI with precise values, units, per-parameter reset, and collapsible MIDI/lighting controls.
  - [x] Provide tooltips and inline descriptions for each built-in parameter, including fine noise-floor adjustments.
- [x] Hide internal routing names behind advanced details; use From/To device labels and Mono/Left/Right/channel names without changing stored route IDs.
- [x] Remove the Routing overview/route-list sidebar and Details toggle; keep graph editing full-width and technical inspection available through APIs and scripts.
- [x] Smooth WebUI level meters with frame-rate, fast-attack/slow-release animation while retaining bounded polling, immediate error clearing, and reduced-motion support.
- [x] Lay out Volume as horizontally scrollable mixer strips with vertical faders/meters and unity reset; hide effect channels by default without changing saved gain or automation.
- [x] Refine mixer styling with compact custom faders, tightly packed per-channel segmented bars without per-channel text, one averaged reading per strip, and error-only meter status.
  - [x] Fix Firefox collapsing the channel-bar bank; verify mono, stereo, and 16-channel layouts in Firefox and Chromium at desktop/mobile widths.

## WebUI polish - v1.13.1
- [x] Rewrite WebUI copy, the manual, README, and contributor documentation for concise current-operation guidance; remove migration/history narration while preserving technical contracts and safety warnings.
- [x] Unify shared panels, navigation, buttons, fields, file pickers and focus states; improve touch targets, responsive MIDI layout, and Scripts information hierarchy.
- [x] Restyle Effects sliders to match the mixer, move parameter descriptions behind accessible info controls, and preserve expanded mapping/advanced sections across refreshes.
- [x] Replace native disclosure triangles throughout the WebUI with consistent custom chevrons and focus states; polish Soundboard setup/Add panels without changing Play.

## Plugin System and more built-in effects - v1.14
- [x] Implement route-aware DSP idle sleep after 5 seconds for testing, ignoring inputs blocked by Cut/zero gain while keeping audio I/O running; preserve audible tails, clear muted tails on sleep entry, and wake on signal or control changes.
- [x] Implement custom plugin system for effects, allowing users to create and load their own effect modules into the audio engine.
  - [x] Create API
  - [x] Define plugin metadata format (axm)
  - [x] Implement plugin loader in the audio engine
  - [x] Provide example plugin for users to reference
  - [x] Document plugin development process for users
  - [x] Test plugin system with multiple plugins loaded simultaneously
  - [x] Handle plugin unloading and cleanup properly
  - [x] Ensure plugin system is compatible with existing effect system and routing logic
  - [x] Implement versioning for plugin API to allow for future updates without breaking existing plugins
  - [x] Implement plugin sandboxing to prevent faulty plugins from crashing the audio engine
  - [x] Provide a way for plugins to give a custom UI for their parameters:
    - We should be able to dynamically serve custom JS or CSS for the plugin UI, since the HTTP server is custom. We could just add an api endpoint for the webui to grab a plugin's assets, and the plugin custom format can also contain multiple asset files (yet another custom archive format like axu and axb)
  - Native and browser regressions pass and AArch64 binaries/packages build.
- [x] More advanced effects
  - [x] Voice noise cancellation/reduction: stationary spectral noise reduction, not echo cancellation or ML speech separation.
  - [x] Compressor
  - [x] Limiter
  - [x] Parametric EQ
  - [x] Multi-band compressor
  - [x] Correct pitch normalization, transparent zero shift and internal dry/wet delay alignment; bound per-effect DSP state without audio-callback allocation.
  - [x] Fix plugin-host lifecycle linking in meter, display-hotplug and gadget-volume host tests; cover their build rules and verify the full CI test suite, including graph ThreadSanitizer.
  - [x] Make Cut and bypass routing-only operations; skip silent built-in chains while preserving tails and control release, retain direct buffer aliases and avoid clearing unused DSP buffers.
  - DSP/sanitizer, routing, HTTP persistence, MIDI/Lua and WebUI regressions pass.

## v1.15.0
- [x] Warn before reloading configuration when unsaved changes could be lost.
- [x] Fix booting on 2 GB Pi 5 boards by including the required overlay.
- [x] Add browser-side update notifications with channel memory, per-release dismissal, device-wide opt-out, and publication-date-aware release checks.
- [x] Add temperature monitoring, Pi 5 fan controls and curve editor, fan RPM/stall feedback, and graceful shutdown from the onboard power button.
- [x] Add named routing, effects, and volume scenes with WebUI/MIDI recall, gain fades, complete active effect inventories, and backup/restore support.
- [x] Add device health history and charts, per-device audio health and negotiated sample rates, power/throttling warnings, and sanitized diagnostic downloads.
- [x] Add routing undo/redo, unsaved-change indicators, and navigation protection; improve health and scene UI layout.
- [x] Render the routing graph at display pixel density on high-DPI screens and provide an on-demand UI preview workflow.
- [x] Add stereo routing with split/merge operations instead of requiring separate mono connections.
- [x] Improve plugin isolation for stock Pi kernels without Landlock; support up to 32 typed controls and metadata-defined help, units, steps, and choices, with plugin effects in the graph Add menu.
- [x] Optimize pitch FFT/window processing and add High, Balanced, and Economy quality modes.
- [x] Add opt-in device telemetry with bounded persistent queuing and browser-assisted uploads, plus public daily aggregate graphs that exclude installation identifiers and raw payloads.
- [x] Improve build and runtime performance with board-specific CPU tuning, stripped packaged binaries, configuration caching, faster framebuffer rendering, and reduced boot/logging delays.
- [x] Bundle graph assets and fonts for offline use.

## Planned - v1.16
- [x] Expand the development documentation beyond telemetry setup, covering the key workflows and systems contributors need to build, test, and extend the project.
- [x] Investigate using LLVM to compile the audio graph into platform-specific native code; compare performance and reliability with the current instruction-based graph, and evaluate graph-change recompilation and whether running the compiled graph in a separately supervised process with IPC for parameters/meters is worthwhile.

## Stabilization - v1.17
- [x] Centralize persistent settings under `/audiox/config/` and separate script definitions from MIDI settings.
  - [x] Include script-owned data in backups, recover interrupted restores, and keep telemetry device-local.
  - [x] Use atomic MIDI/effect/routing and HTTP file replacement; extend newer-schema overwrite protection to raw Lua and HTTP writes.
- [ ] Validate settings on a populated physical Pi: staged USB settings, routing, scripts and options, scenes, Bluetooth pairing, backup/restore, and reboot persistence.
- [ ] Gate firmware trial confirmation on audio readiness and ongoing audio-thread progress, not only 60 seconds of main-loop uptime (`src/init/recovery.cpp`, `src/init/init.cpp`, `src/audio/processing.cpp`). Test a responsive main loop with a stalled audio thread.
- [ ] Reject incomplete rollback boot backups and preserve trial/backup state when restoration is incomplete (`include/boot_recovery.hpp`). Required boot files currently may be skipped during both backup and revert.
- [ ] Fault-inject interruption at update, rollback, configuration-save, and restore persistence boundaries; validate FAT boot-partition power-loss behavior on hardware before declaring 2.0 stable.

## Pre-2.0 validation
- [ ] Have a friend use the setup daily for a week and report issues, feedback, or suggestions.
- [ ] Test the framebuffer UI on a Pi 5 with a connected screen, including rendering, touch interaction, and display hotplug.
- [x] Validate USB MIDI passthrough with physical hosts and multichannel audio.
- [x] Validate Lua keyboard and LED features with physical hardware.
- [x] Validate plugin sandbox support and performance on physical Pi devices with stock kernels.
- [x] Listen to physical Pi audio and validate realtime performance.
- [x] Deploy the telemetry collector at telemetry.audiox.wklab.net and validate real consent, reconnect/reboot persistence, and upload behavior on both installations.
- [x] Validate pitch quality modes and worst-block X-run behavior with real audio on Pi hardware.
- [ ] Validate power/throttling telemetry on physical Pi 4 and Pi 5 hardware, and Pi 5 fan RPM/stall feedback and onboard power-button shutdown.
- [ ] Validate live WebUI/MIDI scene recall with different effect inventories, audible transition quality, and recall latency on hardware.
- [x] Validate audio-device disconnect/reconnect health and the offline WebUI on the deployed appliance.
- [x] Validate X-run reduction, recovery interruptions, and latency tradeoffs on Raspberry Pi hardware under representative workloads.


## Release - v2.0+
- [x] Version configuration formats to protect settings across updates and support backup/restore.
  - [x] Add schema versioning to USB settings, routing, MIDI, effects, volumes, and Bluetooth pairing; refuse unsupported newer versions.
  - [x] Validate configuration schema versions when creating or staging a backup.
- [ ] Stabilize the audio thread under various load conditions and ensure low-latency performance. (maybe dynamic buffer sizing)
  - [x] Add adaptive ALSA period targets after five X-runs in 30 seconds; downshift after 30/60 seconds stable, with 5/10/15-minute in-memory backoff after failed downshifts. Preserve the fixed DSP block size and expose buffer telemetry.
  - [x] Prime enlarged playback queues before startup/recovery; ignore the first runtime minute and 10 seconds after PCM buffer transitions for adaptation and health warnings, while retaining diagnostic totals.
  - [x] Add compile-time-gated, bounded System-page tests for audio-thread overload and whole-system CPU contention; leave normal builds disabled by default.
- [ ] optimize things
  - [x] Strip shipped binaries/libraries in initramfs staging and build with board-specific `-mcpu`.
  - [x] Cache parsed config.txt / midi_map.txt + fx_params.txt behind stat fingerprints.
  - [x] Framebuffer UI: row-fill rects, cached pre-scaled logo, idle frame cap.
  - [x] Throttle log fsyncs on the heartbeat; remove fixed boot sleeps.
- [ ] Harden HTTP API (auth/ACL, size limits review, clearer error payloads)


## Future
- [ ] Post-2.0: Support a dedicated USB keyboard connected directly to the Pi as a controller for every MIDI-mappable action, including soundboard playback, effects, scenes, and scripts.
  - [ ] Translate keyboard input into the shared controller-event/mapping system so keyboard and MIDI bindings use the same mapping controls, without duplicate map buttons or parallel action implementations.
  - [ ] Handle key press/release, held keys, repeat suppression, and disconnect cleanup so actions cannot remain stuck.
- [ ] Post-2.0: Create a lightweight desktop companion app with keyboard shortcuts that trigger actions on the Pi through an API.
  - [ ] Expose controller-event injection through the same shared mapping/action system as local keyboards and MIDI, with paired/authorized access and release cleanup on connection loss.
- [ ] Option in config to use raspi AUX port as an output source (for people without an audio interface, since custom hardware is still WIP)
- [ ] Create a roadmap for future features and improvements
- [ ] Explore integration with other audio software and hardware, such as DAWs and MIDI controllers
- [ ] Hardware...
  - [ ] Big one, this is probably the first hardware milestone: Power supply for pi (so we don't rely on user having a 1.5A or higher USB port on their computer), and screen, as well as a 3d printed case containing the power regulation, screen, pi, and cooling fan.
  - [ ] Built-in audio interface taking advantage of Pi 5's improved I/O capabilities (or just reverting back to basic 2in/2out if a pi 4 is used), a bunch of 1/4in jacks or maybe mixed 1/4in and 3.5mm jacks for flexibility
  - [ ] Design modular synth modules.



## Hardware stuff

Design a spec for a modular synth type thing. It will be like a mini-eurorack, using 3.5mm jacks instead of 1/4 inch jacks, and it will be designed to be good, but budget friendly (single pcb for a module and some 3d printed parts). Basically everything will be CV, since you don't want to have to reach through cables to get to a knob. and there will be simple modules that is just an array of knobs and CV outputs. Eventually I might even make haptic software-controlled knobs, so that when a value is changed via software, the knob will move to that position. I could even use brushless and an absolute encoder so that the knob's boundaries and click positions are completely software controlled. This would allow for a lot of flexibility, and would be a really cool feature. But that is a long way off, and I will probably just make simple modules with knobs and CV outputs for now.

Figure out a connector so that modules can click together and transmit power. This will NOT transmit data, because it is all patch cables.

I don't think I could do a grid because that would create ground loops, so I will have to make rows, and then have a special power supply module and side connector module that goes in a column and spreads power to all modules.

This is meant to be compatible with sitting on a desk (horizontal) OR vertical (like eurorack). Each module will have a 3D printed case. The modules click together with some type of power connector, and maybe even some mechanical connectors to increase stability. The spec for a single module is going to be tiny, but if more space is needed, it can just be a double module, or a triple module, etc.

I will probably not start with the module containing the pi because it will be the most advanced one. Probably start with power supply and a basic mixer or something to test out the spec (will probably need revision).

## Notes to self

- Pi 4 is the first real target (because that is what I have, but software only)
- Pi 5 is the nicer target (for the future of the project, hardware will depend on this, because of multiple I2S busses)
- If the audio pipeline starts fighting me, split it into separate processes later, but only if profiling says that is actually worth the pain
