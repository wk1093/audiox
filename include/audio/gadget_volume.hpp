#pragma once

#include <alsa/asoundlib.h>
#include <atomic>
#include <cstdint>

namespace audiox::usb {

constexpr int kVolumeMin = -96 * 256;
constexpr int kVolumeMax = 0;
constexpr int kVolumeResolution = 256;

float controlGain(long volume, long enabled);

class GadgetVolume {
public:
    std::atomic<float> captureGain{0.0f};
    std::atomic<float> playbackGain{0.0f};

    void poll(int card, unsigned device, uint64_t nowMs);
    void reset();

private:
    int lastCard_ = -1;
    unsigned lastDevice_ = 0;
    uint64_t nextPollMs_ = 0;
    int lastError_ = 0;
};

} // namespace audiox::usb
