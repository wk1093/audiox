#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace audiox::pcm {

inline float clampSample(float sample) {
    if (!std::isfinite(sample)) {
        return 0.0f;
    }
    return std::max(-1.0f, std::min(1.0f, sample));
}

inline int16_t toS16(float sample) {
    const double scaled = static_cast<double>(clampSample(sample)) * 32768.0;
    const long value = std::lrint(scaled);
    return static_cast<int16_t>(std::max(-32768L, std::min(32767L, value)));
}

inline int32_t toS32(float sample) {
    const double scaled = static_cast<double>(clampSample(sample)) * 2147483648.0;
    const long long value = std::llrint(scaled);
    return static_cast<int32_t>(std::max(-2147483648LL, std::min(2147483647LL, value)));
}

}  // namespace audiox::pcm
