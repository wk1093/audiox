#pragma once

#include <cstdint>

namespace audiox::effects {

struct BassBiquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
};

// Calf/TAP tube-warmth saturator: asymmetric transfer curve whose blend sets even harmonics.
struct BassSaturator {
    float kpa = 0, kpb = 0, kna = 0, knb = 0, ap = 0, an = 0, imr = 0, kc = 0, srct = 0, sq = 0,
          pwrq = 0, rest = 0;
    double prevMed = 0, prevOut = 0;
};

struct BassEnhancerState {
    BassBiquad preLow[2]{};
    BassBiquad preHigh[2]{};
    BassBiquad postLow[2]{};
    BassBiquad postHigh[2]{};
    BassSaturator saturator{};
    float scopeHz = 0, floorHz = 0, drive = 0, blend = 0;
    bool initialized = false;
};

void resetBassEnhancer(BassEnhancerState *state);
bool bassEnhancerHasPendingAudio(const BassEnhancerState &state, float threshold = 1.0e-12f);
// Parameters: amount, drive, blend, scope_hz, floor_hz, floor_active, listen, output_gain.
void processBassEnhancer(const float *in, float *out, uint32_t frames, const float *params,
                         BassEnhancerState *state);

} // namespace audiox::effects
