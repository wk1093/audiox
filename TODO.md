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

- [-] Add audio effects (reverb, delay, etc) to the audio engine, and make it so they can be routed to any output, and have their parameters controlled via the HTTP API as well as bindable MIDI CCs and buttons (for toggling effects on/off)
  - [-] Phase Vocoder based pitch shifting (FFT window of 512, 75% overlap, hop size of 128, hann or hanning window. phase unwrapping.)
  - [-] Reverb
  - [ ] Delay
  - [x] Gain
  - [x] Distortion
- [-] Effect midi integration
  - [x] Allow controlling effects with MIDI CCs and buttons
  - [ ] Midi lights for the effect enable/disable buttons

## Very Very Later - v1.6+

- [ ] More advanced effects
  - [ ] Noice gate
  - [ ] Voice noise cancellation/reduction
  - [ ] Compressor
  - [ ] Limiter
  - [ ] Parametric EQ
  - [ ] Multi-band compressor
- [ ] Investigate using LV2 plugins for effects, and if possible, make it so that the user can upload their own LV2 plugins to the device and use them in the audio engine. This would allow for a lot more flexibility and customization for users who want to use their own effects.
- [ ] Investigate lower latency (smaller buffer sizes like 64 or 32 samples instead of 128) to see if we can go below 5ms latency.
- [ ] Smarter bootloader that can detect bad initramfs and boot into a backup one:
  - [ ] File for storing the number of bad boots, and a feature in the main initramfs that will reset that counter once everything has booted properly. If the bootloader sees that this counter is above 2 or 3, it will boot into a backup initramfs instead of the main one.
  - [ ] Is it possible to add a custom listener or hook into a kernel panic? Maybe automatically make a kernel panic trigger a reboot into the backup initramfs?
- [ ] Somehow be able to update the bootloader itself. This could be done by having the main initram update the bootloader, since the bootloader updates the main initram.

## Release - v2.0+

- [ ] Revisit temporary DHCP for better host plug-and-play on Linux
- [ ] Harden HTTP API (auth/ACL, size limits review, clearer error payloads)
- [ ] Add a "midi passthrough" feature that allows non-mapped midi messages to be passed through via a USB gadget to a connected host. This should be configurable via the WebUI, to also allow passing through messages that are mapped to soundboard clips, but the default is to only pass through non-mapped messages. This feature should also be able to be disabled entirely, which would fully remove the gadget entirely to save USB bandwidth (this is for people using like 6+ in/out audio channels to the gadget, where bandwidth could start to matter)
- [ ] Improve the bootloader to be able to do a bit more like changing boot files, not just the initram. Create a custom format (or just use a tar or something) and a web ui for updating the firmware. Also implement a recovery, where the previous working initramfs will be backed up, and if the bootloader detects that the new one is broken it will boot into the previous one. This is mainly to make it easier to recover from a broken initram (sometimes I cause kernel panics).

## Hardware dream stuff

- Reverse-HAT style board that the Pi sits on top of
- 4 in / 4 out minimum, probably more on the premium boards
- 48V mic input
- A few instrument inputs with proper gain staging
- Headphone and monitor outputs
- Knobs, mixers, patching, the whole thing

## Notes to self

- Pi 4 is the first real target (because that is what I have, but software only)
- Pi 5 is the nicer target (for the future of the project, hardware will depend on this, because of multiple I2S busses)
- If the audio pipeline starts fighting me, split it into separate processes later, but only if profiling says that is actually worth the pain
