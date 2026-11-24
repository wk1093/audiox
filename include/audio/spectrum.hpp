#pragma once

#include "audio/effects/fft.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace audiox::spectrum {

constexpr uint32_t kFftSize = 4096;
constexpr uint32_t kBands = 96;
constexpr float kMinHz = 20.0f;
constexpr float kFloorDb = -120.0f;

inline float maxHz(float sampleRate) { return std::min(20000.0f, sampleRate * 0.5f); }

// Log-spaced band edges from kMinHz to maxHz(sampleRate).
inline float bandEdgeHz(uint32_t edge, float sampleRate) {
    return kMinHz * std::pow(maxHz(sampleRate) / kMinHz, static_cast<float>(edge) / kBands);
}

// Hann-windowed magnitude per log band in dBFS; a full-scale sine reads about 0 dB.
inline void computeBands(const float *samples, float sampleRate, float *bandsDb) {
    static const effects::FftPlan<kFftSize> plan;
    thread_local std::vector<float> data(kFftSize * 2);
    thread_local std::vector<float> magnitude(kFftSize / 2 + 1);
    float windowSum = 0;
    for (uint32_t i = 0; i < kFftSize; ++i) {
        const float window = 0.5f - 0.5f * std::cos(2.0f * 3.14159265358979323846f * i / kFftSize);
        windowSum += window;
        data[i * 2] = samples[i] * window;
        data[i * 2 + 1] = 0;
    }
    plan.transform(data.data(), false);
    const float scale = 2.0f / windowSum;
    for (uint32_t bin = 0; bin <= kFftSize / 2; ++bin)
        magnitude[bin] = std::hypot(data[bin * 2], data[bin * 2 + 1]) * scale;
    const float binHz = sampleRate / kFftSize;
    for (uint32_t band = 0; band < kBands; ++band) {
        const float lo = bandEdgeHz(band, sampleRate) / binHz;
        const float hi = bandEdgeHz(band + 1, sampleRate) / binHz;
        float peak = 0;
        const uint32_t first = static_cast<uint32_t>(std::ceil(lo));
        const uint32_t last = std::min(static_cast<uint32_t>(std::floor(hi)), kFftSize / 2);
        for (uint32_t bin = first; bin <= last; ++bin) peak = std::max(peak, magnitude[bin]);
        if (first > last) {
            // Narrower than one bin: interpolate at the band centre.
            const float centre = std::min(std::sqrt(lo * hi), kFftSize / 2.0f - 1.0f);
            const uint32_t bin = static_cast<uint32_t>(centre);
            const float t = centre - bin;
            peak = magnitude[bin] * (1 - t) + magnitude[bin + 1] * t;
        }
        bandsDb[band] = peak > 0 ? std::max(kFloorDb, 20.0f * std::log10(peak)) : kFloorDb;
    }
}

} // namespace audiox::spectrum
