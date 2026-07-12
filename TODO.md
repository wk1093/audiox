# TODO

This is the rough plan for audiox. Not fancy. Just the stuff I need to get done before the good part starts working.

## Right now

- Rewrite the entire codebase to be modular and maintainable, using C++/C with classes (NO MODERN C++ features, just classes)
- framebuffer UI element for RAM and CPU usage, so I can see if the Pi is struggling
- Add a static ffmpeg build to the initramfs so that uploaded audio files can be converted to wav (which I can easily parse and play)
- AUDIO CRACKLING FIX: drift-aware resampling (adaptive SRC based on ring fill level)

## Network + web control status

- [ ] Revisit temporary DHCP for better host plug-and-play on Linux
- [ ] Harden HTTP API (auth/ACL, size limits review, clearer error payloads)
- [ ] Polish UI:
  - [ ] Make svg connection nodes start at edge of device inside of under it, if a connection goes up or down it looks weird
  - [ ] Make it so currently connected node channels are highlighted, and when selecting a node, anything it's connected to is highlighted extra/differently

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
