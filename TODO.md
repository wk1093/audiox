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

- [ ] Improve soundboard UI to be more usable and less confusing
- [ ] Polish UI:
  - [ ] Make svg connection nodes start at edge of device inside of under it, if a connection goes up or down it looks weird
  - [ ] Make it so currently connected node channels are highlighted, and when selecting a node, anything it's connected to is highlighted extra/differently
- [ ] Make soundboard MIDI mapping more flexible
- [ ] Make soundboard able to output midi as well to display lights on a connected controller
- [ ] Volume sliders for all devices, with proper gain control for USB gadget input

## Later - v1.2+

- [ ] Add audio effects (reverb, delay, etc) to the audio engine, and make it so they can be routed to any output, and have their parameters controlled via the HTTP API as well as bindable MIDI CCs and buttons (for toggling effects on/off)
- [ ] Add a static ffmpeg build to the initramfs so that uploaded audio files can be converted to wav (which I can easily parse and play)
- [ ] Allow controlling routes with MIDI CCs and buttons

## Later - v1.3+

- [ ] Revisit temporary DHCP for better host plug-and-play on Linux
- [ ] Harden HTTP API (auth/ACL, size limits review, clearer error payloads)

## Hardware dream stuff

- Reverse-HAT style board that the Pi sits on top of
- 4 in / 4 out minimum, probably more on the premium boards
- 48V mic input
- A few instrument inputs with proper gain staging
- Headphone and monitor outputs
- Knobs, mixers, patching, the whole thing

## Notes to self

- Pi 4 is the first real target (because that is what I have)
- Pi 5 is the nicer target (for the future of the project)
- Pi 3 is only worth it if the performance budget still makes sense (likely limited by CPU headroom for full routing/effects workloads)
- If the audio pipeline starts fighting me, split it into separate processes later, but only if profiling says that is actually worth the pain
