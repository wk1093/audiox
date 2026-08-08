#include "audio/effects/gate.hpp"

#include "defs.hpp"

#include <cmath>

namespace audiox::effects {

namespace {

constexpr float kMinThresholdDb = -80.0f;
constexpr float kMaxThresholdDb = 0.0f;
constexpr float kMinAttackMs = 0.2f;
constexpr float kMaxAttackMs = 100.0f;
constexpr float kMinReleaseMs = 5.0f;
constexpr float kMaxReleaseMs = 600.0f;
constexpr float kMinRangeDb = 0.0f;
constexpr float kMaxRangeDb = 80.0f;
constexpr float kKneeWidthDb = 6.0f;
constexpr float kDbFloor = 1.0e-8f;

inline float clampf(float v, float lo, float hi) {
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

inline float dbToLinear(float db) {
    return powf(10.0f, db / 20.0f);
}

inline float linearToDb(float lin) {
    return 20.0f * log10f((lin > kDbFloor) ? lin : kDbFloor);
}

inline float timeToCoef(float ms) {
    const float samples = (ms * (float)SAMPLE_RATE) / 1000.0f;
    if (samples <= 1.0f) {
        return 0.0f;
    }
    return expf(-1.0f / samples);
}

} // namespace

void processGate(const float *in,
                 float *out,
                 uint32_t frames,
                 float thresholdDb,
                 float attackMs,
                 float releaseMs,
                 float rangeDb,
                 GateState *state) {
    if (!in || !out || !state) {
        return;
    }

    thresholdDb = clampf(thresholdDb, kMinThresholdDb, kMaxThresholdDb);
    attackMs = clampf(attackMs, kMinAttackMs, kMaxAttackMs);
    releaseMs = clampf(releaseMs, kMinReleaseMs, kMaxReleaseMs);
    rangeDb = clampf(rangeDb, kMinRangeDb, kMaxRangeDb);

    float env = (state->envelope > 0.0f) ? state->envelope : 0.0f;
    float gateGain = (state->gain > 0.0f) ? state->gain : 0.0f;

    const float detectorAttackCoef = timeToCoef(attackMs * 0.5f);
    const float detectorReleaseCoef = timeToCoef(releaseMs * 0.5f);
    const float openCoef = timeToCoef(attackMs);
    const float closeCoef = timeToCoef(releaseMs);

    const float floorGain = dbToLinear(-rangeDb);
    const float kneeStartDb = thresholdDb - kKneeWidthDb;
    const float kneeEndDb = thresholdDb + kKneeWidthDb;

    for (uint32_t frame = 0; frame < frames; ++frame) {
        const float sample = in[frame];
        const float level = fabsf(sample);

        if (level > env) {
            env = detectorAttackCoef * env + (1.0f - detectorAttackCoef) * level;
        } else {
            env = detectorReleaseCoef * env + (1.0f - detectorReleaseCoef) * level;
        }

        const float envDb = linearToDb(env);
        float target = floorGain;

        if (envDb >= kneeEndDb) {
            target = 1.0f;
        } else if (envDb > kneeStartDb) {
            const float t = (envDb - kneeStartDb) / (kneeEndDb - kneeStartDb);
            target = floorGain + ((1.0f - floorGain) * t);
        }

        if (target > gateGain) {
            gateGain = openCoef * gateGain + (1.0f - openCoef) * target;
        } else {
            gateGain = closeCoef * gateGain + (1.0f - closeCoef) * target;
        }

        out[frame] = sample * gateGain;
    }

    state->envelope = env;
    state->gain = gateGain;
}

} // namespace audiox::effects
