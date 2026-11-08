#pragma once

#include "audio/effects/slot.hpp"
#include "audio/effects/pitch.hpp"
#include "audio/effects/reverb.hpp"
#include "audio/effects/gate.hpp"
#include "audio/effects/dynamics.hpp"
#include "audio/effects/equalizer.hpp"
#include "audio/effects/noise_reduction.hpp"
#include "defs.hpp"
#include <cmath>

namespace audiox::effects {

inline bool builtinHasPendingAudio(uint8_t type, const void *memory, float threshold) {
    if (!memory) return false;
    switch (type) {
    case EFFECT_PITCH: {
        const auto &state = *static_cast<const PitchState *>(memory);
        for (unsigned i = 0; i < PitchState::FFT_SIZE; ++i)
            if (!(std::fabs(state.inFifo[i]) <= threshold) || !(std::fabs(state.outFifo[i]) <= threshold) ||
                !(std::fabs(state.dryDelay[i]) <= threshold) || !(std::fabs(state.outputAccum[i]) <= threshold))
                return true;
        return !(std::fabs(state.wetLpfZ) <= threshold);
    }
    case EFFECT_REVERB: {
        const auto &state = *static_cast<const ReverbState *>(memory);
        for (const auto &buffer : state.comb)
            for (float sample : buffer)
                if (!(std::fabs(sample) <= threshold)) return true;
        for (float sample : state.combFilter)
            if (!(std::fabs(sample) <= threshold)) return true;
        for (const auto &buffer : state.allpass)
            for (float sample : buffer)
                if (!(std::fabs(sample) <= threshold)) return true;
        return false;
    }
    case EFFECT_LIMITER: return limiterHasPendingAudio(*static_cast<const LimiterState *>(memory), threshold);
    case EFFECT_MULTIBAND:
        return multibandCompressorHasPendingAudio(*static_cast<const MultibandCompressorState *>(memory), threshold);
    case EFFECT_EQ: return equalizerHasPendingAudio(*static_cast<const EqualizerState *>(memory), threshold);
    case EFFECT_NOISE_REDUCTION:
        return noiseReductionHasPendingAudio(*static_cast<const NoiseReductionState *>(memory), threshold);
    default: return false;
    }
}

inline bool builtinNeedsSilentProcessing(const SlotParams &params, const void *memory) {
    if (builtinHasPendingAudio(params.type, memory, 1.0e-12f)) return true;
    if (!memory) return false;
    switch (params.type) {
    case EFFECT_GATE: {
        const auto &state = *static_cast<const GateState *>(memory);
        if (state.envelope > 1e-12f) return true;
        const float floor = powf(10.0f, -params.values[3] / 20.0f);
        const float ms = floor > state.gain ? params.values[1] : params.values[2];
        const float samples = ms * float(SAMPLE_RATE) / 1000.0f;
        const float coefficient = samples <= 1 ? 0 : expf(-1 / samples);
        return coefficient * state.gain + (1 - coefficient) * floor != state.gain;
    }
    case EFFECT_COMPRESSOR: return static_cast<const CompressorState *>(memory)->reductionDb > 1e-12;
    case EFFECT_LIMITER: {
        const auto &state = *static_cast<const LimiterState *>(memory);
        const double release = exp(-1000.0 / (params.values[1] * SAMPLE_RATE));
        return std::fabs(state.gain - 1.0) > 1e-12 && 1 + release * (state.gain - 1) != state.gain;
    }
    case EFFECT_MULTIBAND:
        for (double reduction : static_cast<const MultibandCompressorState *>(memory)->reductionDb)
            if (reduction > 1e-12) return true;
        return false;
    case EFFECT_EQ: return static_cast<const EqualizerState *>(memory)->transitioning;
    default: return false;
    }
}
} // namespace audiox::effects
