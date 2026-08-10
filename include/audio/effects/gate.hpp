#pragma once

#include <cstdint>

namespace audiox::effects {

struct GateState {
    float envelope;
    float gain;
};

void processGate(const float *in,
                 float *out,
                 uint32_t frames,
                 float thresholdDb,
                 float attackMs,
                 float releaseMs,
                 float rangeDb,
                 GateState *state);

} // namespace audiox::effects
