#include "audio/effects/slot.hpp"

#include "defs.hpp"

#include "audio/effects/distortion.hpp"
#include "audio/effects/gate.hpp"
#include "audio/effects/gain.hpp"
#include "audio/effects/pitch.hpp"
#include "audio/effects/reverb.hpp"
#include "audio/effects/state_bank.hpp"
#include "audio/effects/dynamics.hpp"
#include "audio/effects/equalizer.hpp"
#include "audio/effects/noise_reduction.hpp"
#include "audio/plugin_registry.hpp"
#include "audio/plugin_host.hpp"

#include <math.h>
#include <string.h>

namespace audiox::effects {

namespace {

constexpr uint8_t kGainParamCount = 1;
constexpr EffectParamSpec kGainParams[kGainParamCount] = {
    {"level", "Level", 0.0f, 4.0f, 1.0f},
};

constexpr uint8_t kDistortionParamCount = 4;
constexpr EffectParamSpec kDistortionParams[kDistortionParamCount] = {
    {"input_gain", "Input Gain", 0.0f, 4.0f, 1.0f},
    {"drive", "Drive", 0.0f, 8.0f, 4.0f},
    {"clip", "Clip", 0.05f, 1.0f, 0.35f},
    {"output_gain", "Output Gain", 0.0f, 4.0f, 1.0f},
};

constexpr uint8_t kPitchParamCount = 7;
constexpr EffectParamSpec kPitchParams[kPitchParamCount] = {
    {"input_gain", "Input Gain", 0.0f, 4.0f, 1.0f},
    {"semitones", "Semitones", -12.0f, 12.0f, 0.0f},
    {"mix", "Mix", 0.0f, 1.0f, 1.0f},
    {"output_gain", "Output Gain", 0.0f, 4.0f, 1.0f},
    {"noise_floor", "Noise Floor", 0.0001f, 0.02f, 0.0010f},
    {"wet_lpf_hz", "Wet LPF Hz", 1000.0f, 18000.0f, 12000.0f},
    {"quality", "Quality", 0.0f, 2.0f, 0.0f},
};

constexpr uint8_t kReverbParamCount = 4;
constexpr EffectParamSpec kReverbParams[kReverbParamCount] = {
    {"input_gain", "Input Gain", 0.0f, 4.0f, 1.0f},
    {"feedback", "Feedback", 0.05f, 0.95f, 0.64f},
    {"damping", "Damping", 0.0f, 1.0f, 0.48f},
    {"wet_mix", "Wet Mix", 0.0f, 1.0f, 0.26f},
};

constexpr uint8_t kGateParamCount = 4;
constexpr EffectParamSpec kGateParams[kGateParamCount] = {
    {"threshold_db", "Threshold dB", -80.0f, 0.0f, -62.0f},
    {"attack_ms", "Attack ms", 0.2f, 100.0f, 2.0f},
    {"release_ms", "Release ms", 5.0f, 600.0f, 160.0f},
    {"range_db", "Range dB", 0.0f, 80.0f, 30.0f},
};

constexpr uint8_t kCutParamCount = 1;
constexpr EffectParamSpec kCutParams[kCutParamCount] = {
    {"channels", "Channels", 1.0f, 16.0f, 2.0f},
};
constexpr EffectParamSpec kCompressorParams[] = {
    {"threshold_db", "Threshold dB", -60, 0, -18},
    {"ratio", "Ratio", 1, 20, 4},
    {"attack_ms", "Attack ms", .1f, 100, 10},
    {"release_ms", "Release ms", 5, 1000, 120},
    {"knee_db", "Knee dB", 0, 24, 6},
    {"makeup_db", "Makeup dB", -12, 24, 0},
    {"mix", "Mix", 0, 1, 1},
};
constexpr EffectParamSpec kLimiterParams[] = {
    {"ceiling_db", "Ceiling dB", -24, 0, -1},
    {"release_ms", "Release ms", 5, 1000, 80},
    {"lookahead_ms", "Lookahead ms", 0, 10, 3},
    {"input_gain", "Input Gain", 0, 4, 1},
};
constexpr EffectParamSpec kEqParams[] = {
    {"low_hz", "Low Frequency Hz", 40, 1000, 150},
    {"low_gain_db", "Low Gain dB", -18, 18, 0},
    {"mid_hz", "Mid Frequency Hz", 200, 8000, 1000},
    {"mid_gain_db", "Mid Gain dB", -18, 18, 0},
    {"high_hz", "High Frequency Hz", 1000, 18000, 6000},
    {"high_gain_db", "High Gain dB", -18, 18, 0},
    {"q", "Q", .2f, 10, 1},
    {"output_gain", "Output Gain", 0, 4, 1},
};
constexpr EffectParamSpec kMultibandParams[] = {
    {"low_hz", "Low Crossover Hz", 80, 1000, 200},
    {"high_hz", "High Crossover Hz", 1000, 10000, 2500},
    {"low_threshold_db", "Low Threshold dB", -60, 0, -18},
    {"mid_threshold_db", "Mid Threshold dB", -60, 0, -18},
    {"high_threshold_db", "High Threshold dB", -60, 0, -18},
    {"ratio", "Ratio", 1, 20, 4},
    {"release_ms", "Release ms", 5, 1000, 120},
    {"makeup_db", "Makeup dB", -12, 24, 0},
};
constexpr EffectParamSpec kNoiseParams[] = {
    {"reduction_db", "Reduction dB", 0, 30, 12},
    {"noise_floor_db", "Noise Floor dB", -90, -20, -55},
    {"adaptation", "Adaptation", 0, 1, .1f},
    {"smoothing", "Smoothing", 0, 1, .7f},
    {"mix", "Mix", 0, 1, 1},
    {"output_gain", "Output Gain", 0, 4, 1},
};

constexpr EffectTypeSpec kTypeSpecs[] = {
    {EFFECT_GAIN, kGainParams, kGainParamCount},
    {EFFECT_DISTORTION, kDistortionParams, kDistortionParamCount},
    {EFFECT_PITCH, kPitchParams, kPitchParamCount},
    {EFFECT_REVERB, kReverbParams, kReverbParamCount},
    {EFFECT_GATE, kGateParams, kGateParamCount},
    {EFFECT_CUT, kCutParams, kCutParamCount},
    {EFFECT_COMPRESSOR, kCompressorParams, 7},
    {EFFECT_LIMITER, kLimiterParams, 4},
    {EFFECT_EQ, kEqParams, 8},
    {EFFECT_MULTIBAND, kMultibandParams, 8},
    {EFFECT_NOISE_REDUCTION, kNoiseParams, 6},
};

inline float clampValue(float value, float minValue, float maxValue) {
    if (value < minValue) {
        return minValue;
    }
    if (value > maxValue) {
        return maxValue;
    }
    return value;
}

const char *resolveParamAlias(uint8_t type, const char *name) {
    if (!name || !name[0]) {
        return nullptr;
    }
    if (audiox::plugin::isPluginType(type) || type >= EFFECT_COMPRESSOR)
        return name;
    if (strcmp(name, "gain") == 0) {
        if (type == EFFECT_GAIN) {
            return "level";
        }
        if (type == EFFECT_GATE) {
            return "threshold_db";
        }
        return "input_gain";
    }
    if (strcmp(name, "drive") == 0) {
        if (type == EFFECT_PITCH) {
            return "semitones";
        }
        if (type == EFFECT_REVERB) {
            return "feedback";
        }
        if (type == EFFECT_GATE) {
            return "attack_ms";
        }
        return "drive";
    }
    if (strcmp(name, "clip") == 0) {
        if (type == EFFECT_PITCH) {
            return "mix";
        }
        if (type == EFFECT_REVERB) {
            return "damping";
        }
        if (type == EFFECT_GATE) {
            return "release_ms";
        }
        return "clip";
    }
    if (strcmp(name, "output") == 0) {
        if (type == EFFECT_REVERB) {
            return "wet_mix";
        }
        if (type == EFFECT_GATE) {
            return "range_db";
        }
        return "output_gain";
    }
    return name;
}

StateBank<ReverbState> reverbStates;
StateBank<GateState> gateStates;
StateBank<CompressorState> compressorStates;
StateBank<LimiterState> limiterStates;
StateBank<MultibandCompressorState> multibandStates;
StateBank<EqualizerState> equalizerStates;
StateBank<NoiseReductionState> noiseStates;
struct ActiveSlot {
    uint8_t type{};
    bool enabled{};
};
StateBank<ActiveSlot> activeSlots;
ReverbState *getReverbStateFor(const char *effectId, uint8_t channel, bool create = true) {
    return reverbStates.find(effectId, channel, create);
}

GateState *getGateStateFor(const char *effectId, uint8_t channel, bool create = true) {
    return gateStates.find(effectId, channel, create);
}

} // namespace

uint8_t effectTypeFromString(const char *text) {
    if (!text) {
        return EFFECT_GAIN;
    }
    if (audiox::plugin::validTypeName(text))
        return audiox::plugin::reserveType(text);
    if (strcmp(text, "gain") == 0) {
        return EFFECT_GAIN;
    }
    if (strcmp(text, "distortion") == 0) {
        return EFFECT_DISTORTION;
    }
    if (strcmp(text, "pitch") == 0) {
        return EFFECT_PITCH;
    }
    if (strcmp(text, "reverb") == 0) {
        return EFFECT_REVERB;
    }
    if (strcmp(text, "gate") == 0) {
        return EFFECT_GATE;
    }
    if (strcmp(text, "cut") == 0) {
        return EFFECT_CUT;
    }
    if (!strcmp(text, "compressor"))
        return EFFECT_COMPRESSOR;
    if (!strcmp(text, "limiter"))
        return EFFECT_LIMITER;
    if (!strcmp(text, "eq"))
        return EFFECT_EQ;
    if (!strcmp(text, "multiband"))
        return EFFECT_MULTIBAND;
    if (!strcmp(text, "noise_reduction"))
        return EFFECT_NOISE_REDUCTION;
    return EFFECT_GAIN;
}

const char *effectTypeToString(uint8_t type) {
    if (audiox::plugin::isPluginType(type))
        return audiox::plugin::nameFor(type);
    switch (type) {
    case EFFECT_DISTORTION:
        return "distortion";
    case EFFECT_PITCH:
        return "pitch";
    case EFFECT_REVERB:
        return "reverb";
    case EFFECT_GATE:
        return "gate";
    case EFFECT_CUT:
        return "cut";
    case EFFECT_COMPRESSOR:
        return "compressor";
    case EFFECT_LIMITER:
        return "limiter";
    case EFFECT_EQ:
        return "eq";
    case EFFECT_MULTIBAND:
        return "multiband";
    case EFFECT_NOISE_REDUCTION:
        return "noise_reduction";
    case EFFECT_GAIN:
    default:
        return "gain";
    }
}
bool isBuiltinEffectType(uint8_t type) { return type <= EFFECT_NOISE_REDUCTION; }

const EffectTypeSpec *effectTypeSpecFor(uint8_t type) {
    if (audiox::plugin::isPluginType(type))
        return audiox::plugin::specFor(type);
    for (const EffectTypeSpec &spec : kTypeSpecs) {
        if (spec.type == type) {
            return &spec;
        }
    }
    return &kTypeSpecs[0];
}

const EffectParamSpec *effectParamSpecFor(uint8_t type, const char *paramName, uint8_t *outIndex) {
    const EffectTypeSpec *spec = effectTypeSpecFor(type);
    const char *resolvedName = resolveParamAlias(type, paramName);
    if (!resolvedName) {
        return nullptr;
    }

    for (uint8_t i = 0; i < spec->paramCount; ++i) {
        if (strcmp(spec->params[i].name, resolvedName) == 0) {
            if (outIndex) {
                *outIndex = i;
            }
            return &spec->params[i];
        }
    }
    return nullptr;
}

int setSlotParamValue(SlotParams *params, const char *paramName, float value) {
    if (!params || !paramName || !paramName[0] || !isfinite(value)) {
        return RET_ERR;
    }

    uint8_t index = 0;
    const EffectParamSpec *spec = effectParamSpecFor(params->type, paramName, &index);
    if (!spec || index >= EFFECT_PARAM_MAX) {
        return RET_ERR;
    }

    params->values[index] = clampValue(value, spec->minValue, spec->maxValue);
    if ((params->type == EFFECT_CUT && index == 0) ||
        (params->type == EFFECT_PITCH && index == 6)) {
        params->values[index] = roundf(params->values[index]);
    }
    return RET_OK;
}

int setSlotParamNormalized(SlotParams *params, const char *paramName, float normalized) {
    if (!params || !paramName || !paramName[0] || !isfinite(normalized)) {
        return RET_ERR;
    }

    uint8_t index = 0;
    const EffectParamSpec *spec = effectParamSpecFor(params->type, paramName, &index);
    if (!spec || index >= EFFECT_PARAM_MAX) {
        return RET_ERR;
    }

    normalized = clampValue(normalized, 0.0f, 1.0f);
    const float value = spec->minValue + (spec->maxValue - spec->minValue) * normalized;
    params->values[index] = ((params->type == EFFECT_CUT && index == 0) ||
                            (params->type == EFFECT_PITCH && index == 6)) ? roundf(value) : value;
    return RET_OK;
}

int getSlotParamValue(const SlotParams &params, const char *paramName, float *outValue) {
    if (!paramName || !paramName[0] || !outValue) {
        return RET_ERR;
    }

    uint8_t index = 0;
    const EffectParamSpec *spec = effectParamSpecFor(params.type, paramName, &index);
    if (!spec || index >= EFFECT_PARAM_MAX) {
        return RET_ERR;
    }

    const float value = clampValue(params.values[index], spec->minValue, spec->maxValue);
    *outValue = (params.type == EFFECT_PITCH && index == 6) ? roundf(value) : value;
    return RET_OK;
}

void setSlotDefaultsForType(SlotParams *params, uint8_t type) {
    if (!params) {
        return;
    }

    memset(params->values, 0, sizeof(params->values));
    const EffectTypeSpec *spec = effectTypeSpecFor(type);
    const uint8_t limit =
        (spec->paramCount > EFFECT_PARAM_MAX) ? EFFECT_PARAM_MAX : spec->paramCount;
    for (uint8_t i = 0; i < limit; ++i) {
        params->values[i] = spec->params[i].defaultValue;
    }
}

void clampSlotParams(SlotParams *params) {
    if (!params) {
        return;
    }

    params->enabled = params->enabled ? 1U : 0U;
    if (!isBuiltinEffectType(params->type) && !audiox::plugin::isPluginType(params->type)) {
        params->type = EFFECT_GAIN;
    }

    const EffectTypeSpec *spec = effectTypeSpecFor(params->type);
    const uint8_t limit =
        (spec->paramCount > EFFECT_PARAM_MAX) ? EFFECT_PARAM_MAX : spec->paramCount;
    for (uint8_t i = 0; i < limit; ++i) {
        params->values[i] =
            isfinite(params->values[i])
                ? clampValue(params->values[i], spec->params[i].minValue, spec->params[i].maxValue)
                : spec->params[i].defaultValue;
    }
    if (params->type == EFFECT_CUT) {
        params->values[0] = roundf(params->values[0]);
    }
    if (params->type == EFFECT_PITCH) params->values[6] = roundf(params->values[6]);
    for (uint8_t i = limit; i < EFFECT_PARAM_MAX; ++i) {
        params->values[i] = 0.0f;
    }
}

bool slotHasPendingAudio(const char *effectId, uint8_t channel, const SlotParams &params,
                         float threshold) {
    if (!params.enabled)
        return false;
    if (audiox::plugin::isPluginType(params.type))
        return params.pluginHost && params.pluginHost->hasPendingAudio(threshold);
    switch (params.type) {
    case EFFECT_PITCH:
        return pitchHasPendingAudio(effectId, channel, threshold);
    case EFFECT_REVERB: {
        const ReverbState *state = getReverbStateFor(effectId, channel, false);
        if (!state)
            return false;
        for (const auto &buffer : state->comb) {
            for (float sample : buffer) {
                if (!(fabsf(sample) <= threshold))
                    return true;
            }
        }
        for (float sample : state->combFilter) {
            if (!(fabsf(sample) <= threshold))
                return true;
        }
        for (const auto &buffer : state->allpass) {
            for (float sample : buffer) {
                if (!(fabsf(sample) <= threshold))
                    return true;
            }
        }
        return false;
    }
    case EFFECT_COMPRESSOR:
        return false;
    case EFFECT_LIMITER: {
        const auto *state = limiterStates.find(effectId, channel, false);
        return state && limiterHasPendingAudio(*state, threshold);
    }
    case EFFECT_MULTIBAND: {
        const auto *state = multibandStates.find(effectId, channel, false);
        return state && multibandCompressorHasPendingAudio(*state, threshold);
    }
    case EFFECT_EQ: {
        const auto *state = equalizerStates.find(effectId, channel, false);
        return state && equalizerHasPendingAudio(*state, threshold);
    }
    case EFFECT_NOISE_REDUCTION: {
        const auto *state = noiseStates.find(effectId, channel, false);
        return state && noiseReductionHasPendingAudio(*state, threshold);
    }
    case EFFECT_GAIN:
    case EFFECT_DISTORTION:
    case EFFECT_GATE:
    case EFFECT_CUT:
        return false;
    default:
        return true;
    }
}

bool slotNeedsSilentProcessing(const char *effectId, uint8_t channel, const SlotParams &params) {
    if (slotHasPendingAudio(effectId, channel, params)) return true;
    // Detector release and coefficient transitions must not freeze during silence.
    if (params.type == EFFECT_GATE) {
        const auto *state = getGateStateFor(effectId, channel, false);
        if (!state) return false;
        if (state->envelope > 1e-12f) return true;
        const float floor = powf(10.0f, -params.values[3] / 20.0f);
        const float ms = floor > state->gain ? params.values[1] : params.values[2];
        const float samples = ms * float(SAMPLE_RATE) / 1000.0f;
        const float coefficient = samples <= 1 ? 0 : expf(-1 / samples);
        return coefficient * state->gain + (1 - coefficient) * floor != state->gain;
    }
    if (params.type == EFFECT_COMPRESSOR) {
        const auto *state = compressorStates.find(effectId, channel, false);
        return state && state->reductionDb > 1e-12;
    }
    if (params.type == EFFECT_LIMITER) {
        const auto *state = limiterStates.find(effectId, channel, false);
        const double release = exp(-1000.0 / (params.values[1] * SAMPLE_RATE));
        return state && fabs(state->gain - 1.0) > 1e-12 &&
            1 + release * (state->gain - 1) != state->gain;
    }
    if (params.type == EFFECT_MULTIBAND) {
        const auto *state = multibandStates.find(effectId, channel, false);
        if (state)
            for (double reduction : state->reductionDb)
                if (reduction > 1e-12) return true;
    }
    if (params.type == EFFECT_EQ) {
        const auto *state = equalizerStates.find(effectId, channel, false);
        return state && state->transitioning;
    }
    return false;
}

void resetSlotAudio(const char *effectId, uint8_t channel) {
    ReverbState *reverb = getReverbStateFor(effectId, channel, false);
    if (reverb)
        *reverb = {};
    resetPitch(effectId, channel);
    GateState *gate = getGateStateFor(effectId, channel, false);
    if (gate)
        *gate = {};
    resetCompressor(compressorStates.find(effectId, channel, false));
    resetLimiter(limiterStates.find(effectId, channel, false));
    resetMultibandCompressor(multibandStates.find(effectId, channel, false));
    resetEqualizer(equalizerStates.find(effectId, channel, false));
    resetNoiseReduction(noiseStates.find(effectId, channel, false));
}

void prepareSlotAudio(const char *effectId, uint8_t channel, const SlotParams &params) {
    // Only stereo builtins own state; Cut and plugins can expose more sockets.
    if (channel >= 2)
        return;
    auto *active = activeSlots.find(effectId, channel);
    if (active->type != params.type || (active->enabled && !params.enabled))
        resetSlotAudio(effectId, channel);
    active->type = params.type;
    active->enabled = params.enabled;
}

void processSlot(const char *effectId, uint8_t channel, const SlotParams &params, const float *in,
                 float *out, uint32_t frames) {
    if (!in || !out) {
        return;
    }
    prepareSlotAudio(effectId, channel, params);
    if (audiox::plugin::isPluginType(params.type)) {
        if (in != out)
            memcpy(out, in, sizeof(float) * frames);
        return;
    }

    if (!params.enabled) {
        if (in == out) {
            return;
        }
        memcpy(out, in, sizeof(float) * frames);
        return;
    }

    SlotParams clamped = params;
    clampSlotParams(&clamped);

    switch (clamped.type) {
    case EFFECT_EQ:
        processEqualizer(in, out, frames, clamped.values[0], clamped.values[1], clamped.values[2],
                         clamped.values[3], clamped.values[4], clamped.values[5], clamped.values[6],
                         clamped.values[7], equalizerStates.find(effectId, channel));
        return;
    case EFFECT_NOISE_REDUCTION:
        processNoiseReduction(in, out, frames, clamped.values[0], clamped.values[1],
                              clamped.values[2], clamped.values[3], clamped.values[4],
                              clamped.values[5], noiseStates.find(effectId, channel));
        return;
    case EFFECT_COMPRESSOR:
        processCompressor(in, out, frames, clamped.values[0], clamped.values[1], clamped.values[2],
                          clamped.values[3], clamped.values[4], clamped.values[5],
                          clamped.values[6], compressorStates.find(effectId, channel));
        return;
    case EFFECT_LIMITER:
        processLimiter(in, out, frames, clamped.values[0], clamped.values[1], clamped.values[2],
                       clamped.values[3], limiterStates.find(effectId, channel));
        return;
    case EFFECT_MULTIBAND:
        processMultibandCompressor(in, out, frames, clamped.values[0], clamped.values[1],
                                   clamped.values[2], clamped.values[3], clamped.values[4],
                                   clamped.values[5], clamped.values[6], clamped.values[7],
                                   multibandStates.find(effectId, channel));
        return;
    case EFFECT_DISTORTION:
        processDistortion(in, out, frames, clamped.values[0], clamped.values[1], clamped.values[2],
                          clamped.values[3]);
        return;
    case EFFECT_PITCH:
        processPitch(effectId, channel, in, out, frames, clamped.values[0], clamped.values[1],
                     clamped.values[2], clamped.values[3], clamped.values[4], clamped.values[5],
                     static_cast<uint8_t>(clamped.values[6]));
        return;
    case EFFECT_REVERB:
        processReverb(in, out, frames, clamped.values[0], clamped.values[1], clamped.values[2],
                      clamped.values[3], getReverbStateFor(effectId, channel));
        return;
    case EFFECT_GATE:
        processGate(in, out, frames, clamped.values[0], clamped.values[1], clamped.values[2],
                    clamped.values[3], getGateStateFor(effectId, channel));
        return;
    case EFFECT_CUT:
        memset(out, 0, sizeof(float) * frames);
        return;
    case EFFECT_GAIN:
    default:
        processGain(in, out, frames, clamped.values[0]);
        return;
    }
}

} // namespace audiox::effects
