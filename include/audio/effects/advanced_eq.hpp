#pragma once

#include <cstdint>

namespace audiox::effects {

constexpr unsigned kAdvancedEqMaxBands = 32;
constexpr unsigned kAdvancedEqParamsPerBand = 4;
// Slot layout: preamp_db, bands, then {type, hz, gain_db, q} per band.
constexpr unsigned kAdvancedEqFirstBandParam = 2;
constexpr unsigned kAdvancedEqParamCount =
    kAdvancedEqFirstBandParam + kAdvancedEqMaxBands * kAdvancedEqParamsPerBand;

enum AdvancedEqFilter : uint8_t {
    AEQ_OFF = 0,
    AEQ_PEAK = 1,
    AEQ_LOW_SHELF = 2,
    AEQ_HIGH_SHELF = 3,
    AEQ_LOW_PASS = 4,
    AEQ_HIGH_PASS = 5,
    AEQ_BAND_PASS = 6,
    AEQ_NOTCH = 7,
    AEQ_ALL_PASS = 8,
    AEQ_FILTER_LAST = AEQ_ALL_PASS,
};

struct AdvancedEqBiquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    double z1 = 0, z2 = 0;
};

struct AdvancedEqChain {
    AdvancedEqBiquad bands[kAdvancedEqMaxBands]{};
    uint8_t active[kAdvancedEqMaxBands]{};
    uint8_t activeCount = 0;
    double gain = 1;
};

struct AdvancedEqState {
    AdvancedEqChain active{};
    AdvancedEqChain transition{};
    float lastParams[kAdvancedEqParamCount]{};
    uint32_t transitionPosition = 0;
    uint32_t transitionLength = 0;
    bool initialized = false;
    bool transitioning = false;
};

void resetAdvancedEq(AdvancedEqState *state);
bool advancedEqHasPendingAudio(const AdvancedEqState &state, float threshold = 1.0e-12f);
// RBJ biquad coefficients for one band at the given sample rate. Returns false for an
// identity band (Off, or a zero-gain peak/shelf) that can be skipped.
bool advancedEqDesign(uint8_t type, double hz, double gainDb, double q, double sampleRate,
                      AdvancedEqBiquad *out);
void processAdvancedEq(const float *in, float *out, uint32_t frames, const float *params,
                       AdvancedEqState *state);

} // namespace audiox::effects
