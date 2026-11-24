#include "audio/effects/slot.hpp"
#include "audio/effects/native.hpp"
#include "audio/effects/pending.hpp"

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
#include "audio/effects/advanced_eq.hpp"
#include "audio/effects/bass_enhancer.hpp"
#include "audio/effects/stereo.hpp"
#include "audio/plugin_registry.hpp"
#include "audio/plugin_host.hpp"

#include <math.h>
#include <string.h>

namespace audiox::effects {
namespace { thread_local const DspTable *compiledDsp = nullptr; }
void setCompiledDsp(const DspTable *table) { compiledDsp = table; }

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

#define AEQ_BAND(n, hz) \
    {"band" #n "_type", "Band " #n " Type", 0, AEQ_FILTER_LAST, AEQ_PEAK}, \
    {"band" #n "_hz", "Band " #n " Frequency Hz", 10, 22000, hz}, \
    {"band" #n "_gain_db", "Band " #n " Gain dB", -36, 36, 0}, \
    {"band" #n "_q", "Band " #n " Q", .025f, 40, 1.41f}
// The first ten defaults are the ISO octave centres used by ten-band graphic EQs.
constexpr EffectParamSpec kAdvancedEqParams[] = {
    {"preamp_db", "Preamp dB", -36, 36, 0},
    {"bands", "Bands", 0, kAdvancedEqMaxBands, 10},
    AEQ_BAND(1, 31.5f), AEQ_BAND(2, 63), AEQ_BAND(3, 125), AEQ_BAND(4, 250),
    AEQ_BAND(5, 500), AEQ_BAND(6, 1000), AEQ_BAND(7, 2000), AEQ_BAND(8, 4000),
    AEQ_BAND(9, 8000), AEQ_BAND(10, 16000), AEQ_BAND(11, 1000), AEQ_BAND(12, 1000),
    AEQ_BAND(13, 1000), AEQ_BAND(14, 1000), AEQ_BAND(15, 1000), AEQ_BAND(16, 1000),
    AEQ_BAND(17, 1000), AEQ_BAND(18, 1000), AEQ_BAND(19, 1000), AEQ_BAND(20, 1000),
    AEQ_BAND(21, 1000), AEQ_BAND(22, 1000), AEQ_BAND(23, 1000), AEQ_BAND(24, 1000),
    AEQ_BAND(25, 1000), AEQ_BAND(26, 1000), AEQ_BAND(27, 1000), AEQ_BAND(28, 1000),
    AEQ_BAND(29, 1000), AEQ_BAND(30, 1000), AEQ_BAND(31, 1000), AEQ_BAND(32, 1000),
};
#undef AEQ_BAND
static_assert(sizeof(kAdvancedEqParams) / sizeof(kAdvancedEqParams[0]) == kAdvancedEqParamCount);
static_assert(kAdvancedEqParamCount <= EFFECT_PARAM_MAX);
constexpr EffectParamSpec kBassEnhancerParams[] = {
    {"amount_db", "Amount dB", -20, 20, 0},
    {"drive", "Harmonics", .1f, 10, 8.5f},
    {"blend", "Blend", -10, 10, 0},
    {"scope_hz", "Scope Hz", 10, 250, 100},
    {"floor_hz", "Floor Hz", 10, 120, 20},
    {"floor_active", "Floor Active", 0, 1, 0},
    {"listen", "Listen", 0, 1, 0},
    {"output_gain", "Output Gain", 0, 4, 1},
};
constexpr EffectParamSpec kCrossfeedParams[] = {
    {"fcut_hz", "Cutoff Hz", 300, 2000, 700},
    {"feed_db", "Feed dB", 1, 15, 4.5f},
    {"output_gain", "Output Gain", 0, 4, 1},
};
constexpr EffectParamSpec kMaximizerParams[] = {
    {"threshold_db", "Threshold dB", -30, 0, 0},
    {"ceiling_db", "Ceiling dB", -30, 0, -1},
    {"release_ms", "Release ms", 1, 1000, 25},
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
    {EFFECT_ADVANCED_EQ, kAdvancedEqParams, kAdvancedEqParamCount},
    {EFFECT_BASS_ENHANCER, kBassEnhancerParams, 8},
    {EFFECT_CROSSFEED, kCrossfeedParams, 3},
    {EFFECT_MAXIMIZER, kMaximizerParams, 3},
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
StateBank<AdvancedEqState> advancedEqStates;
StateBank<BassEnhancerState> bassEnhancerStates;
StateBank<CrossfeedState> crossfeedStates;
StateBank<MaximizerState> maximizerStates;
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
    if (!strcmp(text, "advanced_eq"))
        return EFFECT_ADVANCED_EQ;
    if (!strcmp(text, "bass_enhancer"))
        return EFFECT_BASS_ENHANCER;
    if (!strcmp(text, "crossfeed"))
        return EFFECT_CROSSFEED;
    if (!strcmp(text, "maximizer"))
        return EFFECT_MAXIMIZER;
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
    case EFFECT_ADVANCED_EQ:
        return "advanced_eq";
    case EFFECT_BASS_ENHANCER:
        return "bass_enhancer";
    case EFFECT_CROSSFEED:
        return "crossfeed";
    case EFFECT_MAXIMIZER:
        return "maximizer";
    case EFFECT_GAIN:
    default:
        return "gain";
    }
}
bool isBuiltinEffectType(uint8_t type) { return type <= EFFECT_BUILTIN_LAST; }

bool isIntegerEffectParam(uint8_t type, uint8_t index) {
    switch (type) {
    case EFFECT_CUT: return index == 0;
    case EFFECT_PITCH: return index == 6;
    case EFFECT_ADVANCED_EQ:
        return index == 1 || (index >= kAdvancedEqFirstBandParam &&
                              (index - kAdvancedEqFirstBandParam) % kAdvancedEqParamsPerBand == 0);
    case EFFECT_BASS_ENHANCER: return index == 5 || index == 6;
    default: {
        const auto *metadata = audiox::plugin::metadataFor(type);
        return metadata && index < metadata->paramCount && strcmp(metadata->params[index].type, "float");
    }
    }
}

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
    if (isIntegerEffectParam(params->type, index)) params->values[index] = roundf(params->values[index]);
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
    return setSlotParamValue(params, paramName, value);
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
    *outValue = isIntegerEffectParam(params.type, index) && params.type != EFFECT_CUT ? roundf(value) : value;
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
    for (uint8_t i = 0; i < limit; ++i)
        if (isIntegerEffectParam(params->type, i)) params->values[i] = roundf(params->values[i]);
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
    if (!isBuiltinEffectType(params.type)) return true;
    return builtinHasPendingAudio(params.type, compiledStateFor(effectId, channel, params.type, false), threshold);
}

bool slotNeedsSilentProcessing(const char *effectId, uint8_t channel, const SlotParams &params) {
    if (slotHasPendingAudio(effectId, channel, params)) return true;
    return builtinNeedsSilentProcessing(params, compiledStateFor(effectId, channel, params.type, false));
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
    resetAdvancedEq(advancedEqStates.find(effectId, channel, false));
    resetBassEnhancer(bassEnhancerStates.find(effectId, channel, false));
    resetCrossfeed(crossfeedStates.find(effectId, channel, false));
    resetMaximizer(maximizerStates.find(effectId, channel, false));
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

void *compiledStateFor(const char *effectId, uint8_t channel, uint8_t type, bool create) {
    switch (type) {
    case EFFECT_EQ: return equalizerStates.find(effectId, channel, create);
    case EFFECT_NOISE_REDUCTION: return noiseStates.find(effectId, channel, create);
    case EFFECT_COMPRESSOR: return compressorStates.find(effectId, channel, create);
    case EFFECT_LIMITER: return limiterStates.find(effectId, channel, create);
    case EFFECT_MULTIBAND: return multibandStates.find(effectId, channel, create);
    case EFFECT_REVERB: return getReverbStateFor(effectId, channel, create);
    case EFFECT_GATE: return getGateStateFor(effectId, channel, create);
    case EFFECT_PITCH: return pitchStateFor(effectId, channel, create);
    case EFFECT_ADVANCED_EQ: return advancedEqStates.find(effectId, channel, create);
    case EFFECT_BASS_ENHANCER: return bassEnhancerStates.find(effectId, channel, create);
    // Stereo-linked state always lives on channel 0.
    case EFFECT_CROSSFEED: return crossfeedStates.find(effectId, 0, create);
    case EFFECT_MAXIMIZER: return maximizerStates.find(effectId, 0, create);
    default: return nullptr;
    }
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

#ifdef AUDIOX_DSP_RUNTIME_ONLY
    if (!compiledDsp || clamped.type >= kBuiltinDspCount || !compiledDsp->entries[clamped.type]) {
        memset(out, 0, sizeof(float) * frames);
        return;
    }
    void *state = compiledStateFor(effectId, channel, clamped.type);
    const void *plan = clamped.type == EFFECT_PITCH ? &getPitchPlan() : nullptr;
    compiledDsp->entries[clamped.type](in, out, frames, clamped.values, state, plan);
#else
    switch (clamped.type) {
    case EFFECT_ADVANCED_EQ:
        processAdvancedEq(in, out, frames, clamped.values, advancedEqStates.find(effectId, channel));
        return;
    case EFFECT_BASS_ENHANCER:
        processBassEnhancer(in, out, frames, clamped.values,
                            bassEnhancerStates.find(effectId, channel));
        return;
    case EFFECT_CROSSFEED:
        processCrossfeed(StereoIo{{in, in}, {out, out}}, frames, clamped.values,
                         crossfeedStates.find(effectId, 0));
        return;
    case EFFECT_MAXIMIZER:
        processMaximizer(StereoIo{{in, in}, {out, out}}, frames, clamped.values,
                         maximizerStates.find(effectId, 0));
        return;
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
#endif
}

void processStereoSlot(const char *effectId, const SlotParams &params, const float *const in[2],
                       float *const out[2], uint32_t frames) {
    if (!in || !out || !in[0] || !in[1] || !out[0] || !out[1]) return;
    prepareSlotAudio(effectId, 0, params);
    prepareSlotAudio(effectId, 1, params);
    if (!params.enabled || !isStereoLinkedType(params.type)) {
        for (unsigned c = 0; c < 2; ++c)
            if (in[c] != out[c]) memcpy(out[c], in[c], sizeof(float) * frames);
        return;
    }
    SlotParams clamped = params;
    clampSlotParams(&clamped);
    const StereoIo io{{in[0], in[1]}, {out[0], out[1]}};
#ifdef AUDIOX_DSP_RUNTIME_ONLY
    if (!compiledDsp || !compiledDsp->entries[clamped.type]) {
        memset(out[0], 0, sizeof(float) * frames);
        memset(out[1], 0, sizeof(float) * frames);
        return;
    }
    compiledDsp->entries[clamped.type](in[0], out[0], frames, clamped.values,
                                       compiledStateFor(effectId, 0, clamped.type), &io);
#else
    if (clamped.type == EFFECT_CROSSFEED)
        processCrossfeed(io, frames, clamped.values, crossfeedStates.find(effectId, 0));
    else
        processMaximizer(io, frames, clamped.values, maximizerStates.find(effectId, 0));
#endif
}

} // namespace audiox::effects
