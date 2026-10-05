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
- [x] Prefer USB vendor/product identity plus a serial hash for stable audio device IDs; use the USB port path when no serial is exposed, and preserve the ALSA ID for existing-route migration.
- [x] Investigate audio-engine latency changes. The processing block is 128 frames, but PCM setup tries 512- and 256-frame periods before 128; defer tuning until Pi-side latency/xrun measurements are available.
  

## Pre-Pre-Release - v1.7+

- [x] Bluetooth support: controlled by web ui, open a specific page on the webui, and it will start listing out bluetooth devices, and you can click one to pair, and it appears in the graph as a device. Will have a specific device naming, and it's graph elements will be persistent even when it isn't connected, so that when it does connect, it remembers how it was routed the last time it was used. I also need to implement some sort of routing tool like a blank effect, so that I can route to a "fake output" that routes to both wired and bluetooth so that both always work, and we have an in between instead of a bunch of individual connections to both. I guess we can use a gain module set to 1, but that adds extra processing, this blank module should be purely for nice routing in the ui and routing config, but in the audio processing should be compiled out.
- [x] fbui soundboard, for people without a midi controller, set a config option in the webui, and the normal fbui will be replaced with a steamdeck-like soundboard interface (with optional multi-page via tabs and/or swipe)
- [x] Investigate occasional "bad boots" where there is constant underruns in the logs, but a reboot always fixes it.

## Big Refactor and fixes - v1.8+
- [x] Fix versioning system (use 1.8.0-beta and whatnot instead of putting 1.8 dev stuff under 1.7.x), implement an extra version field everywhere (makefile, C++ source, JS source, etc)
- [x] Make a config TUI like the linux kernel makefile has to allow configuring for different systems, environments, and be able to change the currently #define static settings like the buffer size and sample rate.
- [x] Split up code better so that we can partially compile the audio subsystem (the audio graph should be a bit more abstract, and shouldn't know about how devices actually work) so that we can test effects and stuff on my desktop instead of re-uploading to the Pi.
  - [x] Add replaceable source/sink callbacks for desktop/shared-graph builds; keep the default Pi realtime loop on a direct compile-time path after callbacks caused device-side underruns.
  - [x] Extract the fixed-capacity PCM capture ring and SRC cursor state from the ALSA stream object; keep ALSA acquisition in the Pi adapter.
  - [x] Move ALSA capture stream ownership, device attachment/remapping, source gadget identity, and fixed PCM/SRC state out of RuntimeGraph into an explicit capture backend; keep ALSA I/O on the direct Pi path.
  - [x] Extract backend-neutral route mixing, node processing, and effect execution; use the same graph block processor in the Pi runtime and host tests.
- [x] "Cut" effect/feature to mute or unmute a route, with configurable matching input/output channel counts and MIDI/Web UI toggling.
- [x] Run the application as a child of a small PID 1 watchdog; restart it after exit or a stale main-loop heartbeat.

## Pre-release - v1.9+
- [ ] Smarter bootloader that can detect bad initramfs and boot into a backup one:
  - [ ] File for storing the number of bad boots, and a feature in the main initramfs that will reset that counter once everything has booted properly. If the bootloader sees that this counter is above 2 or 3, it will boot into a backup initramfs instead of the main one.
  - [ ] Make it so that the bootloader can update the kernel as well.
  - [ ] Is it possible to add a custom listener or hook into a kernel panic? Maybe automatically make a kernel panic trigger a reboot into the backup initramfs?
- [ ] Somehow be able to update the bootloader itself. This could be done by having the main initram update the bootloader, since the bootloader updates the main initram, but they can't edit themselves. So maybe just update the bootloader first from the main initramfs right when the update is triggered, and then reboot into the new bootloader to update the main initramfs. Or I could make them two separate processes, where you could choose to update one or the other. That is probably better since the bootloader won't be updated as often, and we won't have to require a larger update file. The update file will eventually be a custom format that is kinda like a tar, it contains a bit of metadata, and then two separate files (or just one). The update metadata will contain a version number, and then it will also tell the program if it is a bootloader update, initram update, or both.

# Quick Fixes and features - v1.10+
- [ ] See if we can increase bluetooth quality.
- [ ] Windows volume changing doesn't work properly (volume seems to be locked at 100% even when windows volume is changed, and it is really boosted and distorted even if the volume on the device is reduced). Windows probably does something weird with the gadget that I didn't handle.
- [ ] Investigate occasional kernel faults
- [ ] Investigate using LV2 plugins for effects, and if possible, make it so that the user can upload their own LV2 plugins to the device and use them in the audio engine. This would allow for a lot more flexibility and customization for users who want to use their own effects.
- [ ] More advanced effects
  - [ ] Noise gate
  - [ ] Voice noise cancellation/reduction
  - [ ] Compressor
  - [ ] Limiter
  - [ ] Parametric EQ
  - [ ] Multi-band compressor

## Release - v2.0+

- [ ] IMPORTANT: Create a config versioning system, so that older configs can be upgraded if format is changed. This is important for 2.0+ because it is meant to be a more stable release, and we don't want to break people's configs when they update. This will also allow for more advanced features in the future, like being able to export and import configs between devices.
- [ ] IMPORTANT: Verify windows functionality.
  - [ ] Right now the network gadget is only tested on Linux, and it is possible that it won't work on windows.
  - [ ] Verify that the Audio gadget works on Windows, I checked once, and it didn't show up with the right name ("Showed Sink/Source" instead of "AudioX").
- [ ] Revisit temporary DHCP for better host plug-and-play on Linux
- [ ] Harden HTTP API (auth/ACL, size limits review, clearer error payloads)
- [ ] Add a "midi passthrough" feature that allows non-mapped midi messages to be passed through via a USB gadget to a connected host. This should be configurable via the WebUI, to also allow passing through messages that are mapped to soundboard clips, but the default is to only pass through non-mapped messages. This feature should also be able to be disabled entirely, which would fully remove the gadget entirely to save USB bandwidth (this is for people using like 6+ in/out audio channels to the gadget, where bandwidth could start to matter)
- [ ] Look into dynamic sample size for the audio engine, so that if we notice Xruns or any other issues we can automatically increase the sample size when needed (or at least allow the user to change it via the WebUI). This is mainly for people with a lot of channels or effects that could cause issues.

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
