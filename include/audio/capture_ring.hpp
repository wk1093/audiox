#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace audiox::capture {

template <uint32_t FrameCapacity, uint8_t MaxChannels>
class PcmCaptureRing {
    static_assert(FrameCapacity > 0, "capture ring must have at least one frame");
    static_assert(MaxChannels > 0, "capture ring must have at least one channel");

public:
    void reset() {
        head_ = 0;
        tail_ = 0;
        count_ = 0;
        readFraction_ = 0.0f;
        std::memset(lastSamples_, 0, sizeof(lastSamples_));
    }

    uint32_t count() const { return count_; }
    uint32_t head() const { return head_; }
    uint32_t tail() const { return tail_; }
    float readFraction() const { return readFraction_; }
    void setReadFraction(float value) { readFraction_ = value; }

    uint32_t frameIndex(uint32_t offset) const {
        return (head_ + offset) % FrameCapacity;
    }

    uint32_t readFrameIndex(uint32_t offset) const {
        return (tail_ + offset) % FrameCapacity;
    }

    int16_t* writableFrame(uint32_t offset) {
        return &samples_[static_cast<size_t>(frameIndex(offset)) * MaxChannels];
    }

    int16_t sample(uint32_t frame, uint8_t channel) const {
        return samples_[static_cast<size_t>(frame) * MaxChannels + channel];
    }

    float lastSample(uint8_t channel) const { return lastSamples_[channel]; }
    void setLastSample(uint8_t channel, float value) { lastSamples_[channel] = value; }

    void advanceWritten(uint32_t frames) {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            if (count_ >= FrameCapacity) {
                tail_ = (tail_ + 1U) % FrameCapacity;
                count_ = FrameCapacity - 1U;
            }
            head_ = (head_ + 1U) % FrameCapacity;
            ++count_;
        }
    }

    void push(const int16_t* input, uint8_t channels, uint32_t frames) {
        if (!input || channels == 0 || channels > MaxChannels) {
            return;
        }

        for (uint32_t frame = 0; frame < frames; ++frame) {
            if (count_ >= FrameCapacity) {
                tail_ = (tail_ + 1U) % FrameCapacity;
                count_ = FrameCapacity - 1U;
            }
            int16_t* destination = &samples_[static_cast<size_t>(head_) * MaxChannels];
            const int16_t* source = &input[static_cast<size_t>(frame) * channels];
            for (uint8_t channel = 0; channel < channels; ++channel) {
                destination[channel] = source[channel];
            }
            for (uint8_t channel = channels; channel < MaxChannels; ++channel) {
                destination[channel] = 0;
            }
            head_ = (head_ + 1U) % FrameCapacity;
            ++count_;
        }
    }

    void consume(float sourceStep) {
        readFraction_ += sourceStep;
        while (readFraction_ >= 1.0f && count_ > 0) {
            tail_ = (tail_ + 1U) % FrameCapacity;
            --count_;
            readFraction_ -= 1.0f;
        }
        if (count_ == 0) {
            readFraction_ = 0.0f;
        }
    }

private:
    uint32_t head_ = 0;
    uint32_t tail_ = 0;
    uint32_t count_ = 0;
    float readFraction_ = 0.0f;
    float lastSamples_[MaxChannels] = {};
    int16_t samples_[static_cast<size_t>(FrameCapacity) * MaxChannels] = {};
};

}  // namespace audiox::capture
