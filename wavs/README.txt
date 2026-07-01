Place voice WAV files in this directory before building initramfs.

Required files:
- voice0.wav
- voice1.wav
- voice2.wav
- voice3.wav

Format requirements (current loader):
- RIFF/WAVE PCM (audio format 1)
- 16-bit little-endian samples
- Mono or stereo
- Any sample rate (resampled at runtime to 44100 Hz)

These files are copied into the initramfs at /etc/wavs/.
