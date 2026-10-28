#include "audio/gadget_volume.hpp"

#include <cmath>
#include <cstdio>

namespace audiox::usb {

float controlGain(long volume, long enabled) {
    if (enabled == 0) {
        return 0.0f;
    }
    // The gadget driver exposes an integer step index, not raw USB 1/256 dB.
    if (volume < 0 || volume > (kVolumeMax - kVolumeMin) / kVolumeResolution ||
        enabled != 1) {
        return 0.0f;
    }
    return std::pow(10.0f, static_cast<float>(kVolumeMin + volume * kVolumeResolution) / 5120.0f);
}

static int readControl(snd_ctl_t *ctl, unsigned device, const char *name, long *value) {
    snd_ctl_elem_value_t *element;
    snd_ctl_elem_value_alloca(&element);
    snd_ctl_elem_value_set_interface(element, SND_CTL_ELEM_IFACE_MIXER);
    snd_ctl_elem_value_set_device(element, device);
    snd_ctl_elem_value_set_name(element, name);
    int rc = snd_ctl_elem_read(ctl, element);
    if (rc >= 0) {
        *value = snd_ctl_elem_value_get_integer(element, 0);
    }
    return rc;
}

void GadgetVolume::reset() {
    captureGain.store(0.0f, std::memory_order_relaxed);
    playbackGain.store(0.0f, std::memory_order_relaxed);
    lastCard_ = -1;
    nextPollMs_ = 0;
    lastError_ = 0;
}

void GadgetVolume::poll(int card, unsigned device, uint64_t nowMs) {
    if (card != lastCard_ || device != lastDevice_) {
        reset();
        lastCard_ = card;
        lastDevice_ = device;
    }
    if (card < 0 || nowMs < nextPollMs_) {
        return;
    }
    nextPollMs_ = nowMs + 50;

    char path[32];
    snprintf(path, sizeof(path), "hw:%d", card);
    snd_ctl_t *ctl = nullptr;
    int rc = snd_ctl_open(&ctl, path, SND_CTL_NONBLOCK);
    long captureVolume = 0, captureSwitch = 0, playbackVolume = 0, playbackSwitch = 0;
    if (rc >= 0) rc = readControl(ctl, device, "PCM Capture Volume", &captureVolume);
    if (rc >= 0) rc = readControl(ctl, device, "PCM Capture Switch", &captureSwitch);
    if (rc >= 0) rc = readControl(ctl, device, "PCM Playback Volume", &playbackVolume);
    if (rc >= 0) rc = readControl(ctl, device, "PCM Playback Switch", &playbackSwitch);
    if (ctl) {
        int closeRc = snd_ctl_close(ctl);
        if (rc >= 0) rc = closeRc;
    }
    constexpr long maxVolume = (kVolumeMax - kVolumeMin) / kVolumeResolution;
    if (rc >= 0 && (captureVolume < 0 || captureVolume > maxVolume ||
                   playbackVolume < 0 || playbackVolume > maxVolume ||
                   captureSwitch < 0 || captureSwitch > 1 ||
                   playbackSwitch < 0 || playbackSwitch > 1)) {
        rc = -ERANGE;
    }
    if (rc < 0) {
        captureGain.store(0.0f, std::memory_order_relaxed);
        playbackGain.store(0.0f, std::memory_order_relaxed);
        if (rc != lastError_) {
            printf("[AUDIO] [WARN] USB volume controls %s,%u unavailable: %s; USB audio muted until recovery\n",
                   path, device, snd_strerror(rc));
        }
        lastError_ = rc;
        nextPollMs_ = nowMs + 1000;
        return;
    }
    if (lastError_) {
        printf("[AUDIO] [INFO] USB volume controls recovered on %s,%u\n", path, device);
    }
    lastError_ = 0;
    captureGain.store(controlGain(captureVolume, captureSwitch), std::memory_order_relaxed);
    playbackGain.store(controlGain(playbackVolume, playbackSwitch), std::memory_order_relaxed);
}

} // namespace audiox::usb
