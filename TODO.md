# TODO

This is the rough plan for audiox. Not fancy. Just the stuff I need to get done before the good part starts working.

## Right now

- Clean up the audio path so it is actually ready for routing instead of just playing one-off voices
- Keep the hot path tight: fixed buffers, no dumb allocations, no weird locks if I can avoid them
- Add a real internal routing model so "virtual wires" are not a future panic attack
- Make the config side simple enough that I can edit it by hand without hating myself

## Audio refactor phases

- Phase 0 (in progress): split monolithic audio code into modules without changing behavior
- Phase 1: replace render-path mutex reads with control snapshot swap
- Phase 2: add routing model container + prevalidated execution order (linear path first)
- Phase 3: move render path to node execution (same current audible result)
- Phase 4: add first effect node contract + bypass + smoothed params
- Phase 5: expose routing/effects params to config + touchscreen + MIDI

### Phase 0 checklist

- [x] Create modular headers under `include/audio/` and keep existing API stable
- [x] Convert `include/audio_oss.h` to umbrella include
- [x] Add placeholder router node/bus structs for future DAG work
- [ ] Move runtime control state toward snapshot-friendly layout (no behavior change yet)
- [x] Verify build on debug profile
- [ ] Verify build on target cross-compile profile
- [ ] Example with rerouting a separate USB audio device (a usb mic) to the usb gadget output (this is the first real test of the routing model)

## Next

- Finish UI cleanup so the touchscreen shows routing, inputs, outputs, and status without being cluttered
- Add board profiles for Pi 4 / Pi 5, with Pi 3 treated like a maybe-if-it-has-a-good-day option
- Move model-specific stuff out of random places and into one obvious config/profile path
- Keep audio + UI + config code separated enough that I can change one without breaking everything else

## Later

- Virtual audio cables in the UI
- Mixer paths that can be patched like hardware
- Effects control from touch and MIDI
- Soundboard/preset loading from rootfs
- Better desktop config tool if the touchscreen starts feeling too small for the job
- Support multiple board sizes from the same software stack

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
