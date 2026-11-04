#pragma once

#include <array>
#include <cstdint>

namespace audiox::effects {

struct EqualizerParams {
    float lowHz = 150.0f;
    float lowGainDb = 0.0f;
    float midHz = 1000.0f;
    float midGainDb = 0.0f;
    float highHz = 6000.0f;
    float highGainDb = 0.0f;
    float q = 1.0f;
    float outputGain = 1.0f;
};

struct EqualizerBiquad {
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
    float z1 = 0.0f;
    float z2 = 0.0f;
};

struct EqualizerChain {
    std::array<EqualizerBiquad, 3> bands{};
    float outputGain = 1.0f;
};

struct EqualizerState {
    EqualizerChain active{};
    EqualizerChain transition{};
    uint32_t transitionPosition = 0;
    uint32_t transitionLength = 0;
    uint32_t sampleRate = 0;
    bool initialized = false;
    bool transitioning = false;
};

void resetEqualizer(EqualizerState *state);
bool equalizerHasPendingAudio(const EqualizerState &state, float threshold = 1.0e-12f);
// Three RBJ peaking bands; sample rate is the build's SAMPLE_RATE.
void processEqualizer(const float *in, float *out, uint32_t frames, float lowHz, float lowGainDb,
                      float midHz, float midGainDb, float highHz, float highGainDb, float q,
                      float outputGain, EqualizerState *state);

} // namespace audiox::effects
