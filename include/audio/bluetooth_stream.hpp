#pragma once

#include "defs.hpp"

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

// Concurrent Bluetooth devices with their own audio slots. A Bluetooth link
// index doubles as the slot index on both the input and output side.
#define AUDIO_BT_MAX_LINKS 4
#define AUDIO_BT_PCM_RING_FRAMES 16384

// Decoded A2DP audio from one remote source. The Bluetooth worker pushes
// bursty packets; the audio thread plays them through a jitter buffer that
// primes before playback, rebuffers after an underrun and slowly adjusts the
// read rate to track the sender's clock.
struct BluetoothInputStream {
    // Packed address (bt::packAddress) of the device feeding this slot; 0 when idle.
    std::atomic<uint64_t> peer{0};
    std::atomic<uint32_t> sampleRate{48000};
    std::atomic<uint32_t> generation{0};
    // Write index when the current generation began; the reader restarts there.
    std::atomic<uint32_t> startIndex{0};
    std::atomic<uint32_t> write{0};
    std::atomic<uint32_t> read{0};
    std::atomic<uint32_t> dropped{0};
    std::atomic<uint32_t> underruns{0};
    std::atomic<uint32_t> overflows{0};
    int16_t ring[AUDIO_BT_PCM_RING_FRAMES][2] = {};

    // Audio-thread state.
    uint32_t seenGeneration = 0;
    bool primed = false;
    float readFrac = 0.0f;
    float filteredFill = 0.0f;
    float rateIntegral = 0.0f;

    // Bluetooth worker: begin feeding audio from `address` at `rate` Hz.
    void start(uint64_t address, uint32_t rate) {
        sampleRate.store(rate ? rate : 48000U, std::memory_order_relaxed);
        startIndex.store(write.load(std::memory_order_relaxed), std::memory_order_relaxed);
        generation.fetch_add(1U, std::memory_order_release);
        peer.store(address, std::memory_order_release);
    }

    void stop() {
        peer.store(0, std::memory_order_release);
        startIndex.store(write.load(std::memory_order_relaxed), std::memory_order_relaxed);
        generation.fetch_add(1U, std::memory_order_release);
    }

    // Bluetooth worker: stereo interleaved PCM. Never blocks; drops what does not fit.
    uint32_t push(const int16_t *stereoFrames, uint32_t frames) {
        if (!stereoFrames || frames == 0) {
            return 0;
        }
        const uint32_t writeIndex = write.load(std::memory_order_relaxed);
        const uint32_t readIndex = read.load(std::memory_order_acquire);
        const uint32_t used = writeIndex - readIndex;
        const uint32_t freeFrames = used >= AUDIO_BT_PCM_RING_FRAMES ? 0 : AUDIO_BT_PCM_RING_FRAMES - used;
        const uint32_t accepted = frames < freeFrames ? frames : freeFrames;
        for (uint32_t frame = 0; frame < accepted; ++frame) {
            const uint32_t slot = (writeIndex + frame) % AUDIO_BT_PCM_RING_FRAMES;
            ring[slot][0] = stereoFrames[frame * 2U];
            ring[slot][1] = stereoFrames[frame * 2U + 1U];
        }
        write.store(writeIndex + accepted, std::memory_order_release);
        if (accepted < frames) {
            dropped.fetch_add(frames - accepted, std::memory_order_relaxed);
        }
        return accepted;
    }

    uint32_t targetFill() const {
        // About 120 ms: phones deliver A2DP in irregular bursts.
        const uint32_t rate = sampleRate.load(std::memory_order_relaxed);
        const uint32_t target = (rate ? rate : 48000U) * 3U / 25U;
        return target < BUFFER_FRAMES * 4U ? BUFFER_FRAMES * 4U : target;
    }

    // Audio thread: renders `frames` stereo samples at SAMPLE_RATE.
    void render(float *left, float *right, uint32_t frames) {
        const uint32_t gen = generation.load(std::memory_order_acquire);
        uint32_t readIndex = read.load(std::memory_order_relaxed);
        if (gen != seenGeneration) {
            seenGeneration = gen;
            readIndex = startIndex.load(std::memory_order_relaxed);
            read.store(readIndex, std::memory_order_release);
            primed = false;
            readFrac = 0.0f;
            filteredFill = 0.0f;
            rateIntegral = 0.0f;
        }
        const uint32_t writeIndex = write.load(std::memory_order_acquire);
        uint32_t available = writeIndex - readIndex;
        const uint32_t target = targetFill();

        if (!primed) {
            if (available < target) {
                memset(left, 0, frames * sizeof(float));
                memset(right, 0, frames * sizeof(float));
                return;
            }
            primed = true;
            filteredFill = (float)available;
            rateIntegral = 0.0f;
            readFrac = 0.0f;
        }

        if (available > AUDIO_BT_PCM_RING_FRAMES - 2048U) {
            // The sender got far ahead (e.g. a long stall then a flood); skip back to target latency.
            readIndex = writeIndex - target;
            available = target;
            readFrac = 0.0f;
            filteredFill = (float)target;
            overflows.fetch_add(1U, std::memory_order_relaxed);
        }

        const uint32_t rate = sampleRate.load(std::memory_order_relaxed);
        const float seconds = (float)frames / (float)SAMPLE_RATE;
        filteredFill += ((float)available - filteredFill) * (seconds > 1.0f ? 1.0f : seconds);
        float error = (filteredFill - (float)target) / (float)target;
        if (error > 1.0f) error = 1.0f;
        if (error < -1.0f) error = -1.0f;
        rateIntegral += error * 0.00008f * seconds;
        if (rateIntegral > 0.003f) rateIntegral = 0.003f;
        if (rateIntegral < -0.003f) rateIntegral = -0.003f;
        float adjust = error * 0.003f + rateIntegral;
        if (adjust > 0.005f) adjust = 0.005f;
        if (adjust < -0.005f) adjust = -0.005f;
        const float ratio = ((float)(rate ? rate : 48000U) / (float)SAMPLE_RATE) * (1.0f + adjust);

        float fraction = readFrac;
        uint32_t frame = 0;
        for (; frame < frames; ++frame) {
            const uint32_t offset = (uint32_t)fraction;
            if (offset + 1U >= available) {
                break;
            }
            const uint32_t slotA = (readIndex + offset) % AUDIO_BT_PCM_RING_FRAMES;
            const uint32_t slotB = (slotA + 1U) % AUDIO_BT_PCM_RING_FRAMES;
            const float mix = fraction - (float)offset;
            left[frame] = ((float)ring[slotA][0] + ((float)ring[slotB][0] - (float)ring[slotA][0]) * mix) / 32768.0f;
            right[frame] = ((float)ring[slotA][1] + ((float)ring[slotB][1] - (float)ring[slotA][1]) * mix) / 32768.0f;
            fraction += ratio;
        }
        uint32_t consumed = (uint32_t)fraction;
        if (frame < frames) {
            // Ran dry: fade out the remainder and wait for the buffer to refill.
            const float last[2] = {frame ? left[frame - 1U] : 0.0f, frame ? right[frame - 1U] : 0.0f};
            for (uint32_t i = frame; i < frames; ++i) {
                const float gain = 1.0f - (float)(i - frame + 1U) / 32.0f;
                left[i] = gain > 0.0f ? last[0] * gain : 0.0f;
                right[i] = gain > 0.0f ? last[1] * gain : 0.0f;
            }
            consumed = available;
            fraction = 0.0f;
            primed = false;
            underruns.fetch_add(1U, std::memory_order_relaxed);
        }
        readFrac = fraction - (float)(uint32_t)fraction;
        read.store(readIndex + consumed, std::memory_order_release);
    }
};

// PCM rendered for one Bluetooth headphone/speaker. The audio thread pushes
// fixed blocks; the Bluetooth worker pulls packet-sized chunks and resamples
// slightly to follow the radio's pacing.
struct BluetoothOutputStream {
    std::atomic<uint64_t> peer{0};
    std::atomic<uint32_t> write{0};
    std::atomic<uint32_t> read{0};
    std::atomic<uint32_t> dropped{0};
    std::atomic<int> active{0};
    std::atomic<int> signalActive{0};
    int16_t ring[AUDIO_BT_PCM_RING_FRAMES][2] = {};

    // Bluetooth-worker state.
    float readFraction = 0.0f;
    float rateIntegral = 0.0f;
    float rateRatio = 1.0f;
    float filteredFill = 0.0f;
    uint64_t rateLastUpdateMs = 0;
    uint8_t rateInitialized = 0;

    // Audio thread.
    uint32_t push(const int16_t *stereoFrames, uint32_t frames) {
        if (!stereoFrames || frames == 0 || !active.load(std::memory_order_acquire)) {
            return 0;
        }
        const uint32_t writeIndex = write.load(std::memory_order_relaxed);
        const uint32_t readIndex = read.load(std::memory_order_acquire);
        const uint32_t used = writeIndex - readIndex;
        const uint32_t freeFrames = used >= AUDIO_BT_PCM_RING_FRAMES ? 0 : AUDIO_BT_PCM_RING_FRAMES - used;
        const uint32_t accepted = frames < freeFrames ? frames : freeFrames;
        for (uint32_t frame = 0; frame < accepted; ++frame) {
            const uint32_t slot = (writeIndex + frame) % AUDIO_BT_PCM_RING_FRAMES;
            ring[slot][0] = stereoFrames[frame * 2U];
            ring[slot][1] = stereoFrames[frame * 2U + 1U];
        }
        write.store(writeIndex + accepted, std::memory_order_release);
        if (accepted < frames) {
            dropped.fetch_add(frames - accepted, std::memory_order_relaxed);
        }
        return accepted;
    }

    uint32_t available() const {
        const uint32_t writeIndex = write.load(std::memory_order_acquire);
        const uint32_t readIndex = read.load(std::memory_order_acquire);
        return writeIndex - readIndex;
    }

    // Bluetooth worker: (de)activates the slot and drops anything queued.
    void setActive(bool enable) {
        active.store(0, std::memory_order_release);
        read.store(write.load(std::memory_order_acquire), std::memory_order_release);
        readFraction = 0.0f;
        rateIntegral = 0.0f;
        rateRatio = 1.0f;
        filteredFill = 0.0f;
        rateLastUpdateMs = 0;
        rateInitialized = 0;
        if (enable) {
            active.store(1, std::memory_order_release);
        }
    }

    void updateRate(uint32_t packetFrames, uint64_t nowMs) {
        if (packetFrames == 0 || !active.load(std::memory_order_acquire)) {
            return;
        }
        if (rateLastUpdateMs != 0 && nowMs - rateLastUpdateMs < 10U) {
            return;
        }
        const uint32_t fill = available();
        uint64_t elapsedMs = rateLastUpdateMs == 0 ? 10U : nowMs - rateLastUpdateMs;
        if (elapsedMs > 100U) {
            elapsedMs = 100U;
        }
        rateLastUpdateMs = nowMs;
        if (!rateInitialized) {
            filteredFill = (float)fill;
            rateInitialized = 1U;
        } else {
            const float alpha = 1.0f - expf(-(float)elapsedMs / 80.0f);
            filteredFill += ((float)fill - filteredFill) * alpha;
        }
        const float targetFill = (float)packetFrames * 0.5f + (float)BUFFER_FRAMES * 0.5f;
        const float fillError = (filteredFill - targetFill) / targetFill;
        rateIntegral += fillError * (float)elapsedMs * 0.0000001175f;
        if (rateIntegral > 0.003f) rateIntegral = 0.003f;
        if (rateIntegral < -0.003f) rateIntegral = -0.003f;
        float ratio = 1.0f + fillError * 0.0015f + rateIntegral;
        if (ratio > 1.005f) ratio = 1.005f;
        if (ratio < 0.995f) ratio = 0.995f;
        rateRatio = ratio;
    }

    // Bluetooth worker: returns outputFrames, or 0 if not enough PCM is queued.
    uint32_t resample(int16_t *stereoFrames, uint32_t outputFrames) {
        if (!stereoFrames || outputFrames == 0) {
            return 0;
        }
        const uint32_t readIndex = read.load(std::memory_order_relaxed);
        const uint32_t fill = write.load(std::memory_order_acquire) - readIndex;
        const float ratio = rateRatio;
        const uint32_t neededFrames = (uint32_t)(readFraction + (float)outputFrames * ratio) + 2U;
        if (fill < neededFrames) {
            return 0;
        }
        float fraction = readFraction;
        for (uint32_t frame = 0; frame < outputFrames; ++frame) {
            const uint32_t advance = (uint32_t)fraction;
            const uint32_t slotA = (readIndex + advance) % AUDIO_BT_PCM_RING_FRAMES;
            const uint32_t slotB = (slotA + 1U) % AUDIO_BT_PCM_RING_FRAMES;
            for (uint32_t channel = 0; channel < 2; ++channel) {
                const float a = ring[slotA][channel];
                const float b = ring[slotB][channel];
                float value = a + (b - a) * (fraction - (float)advance);
                if (value > 32767.0f) value = 32767.0f;
                if (value < -32768.0f) value = -32768.0f;
                stereoFrames[frame * 2U + channel] = (int16_t)value;
            }
            fraction += ratio;
        }
        const uint32_t consumed = (uint32_t)fraction;
        readFraction = fraction - (float)consumed;
        read.store(readIndex + consumed, std::memory_order_release);
        return outputFrames;
    }
};
