#include "audio/effects/slot.hpp"

#include "defs.hpp"

#include "audio/effects/distortion.hpp"
#include "audio/effects/gate.hpp"
#include "audio/effects/gain.hpp"
#include "audio/effects/pitch.hpp"
#include "audio/effects/reverb.hpp"

#include <string.h>
#include <string>
#include <unordered_map>

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

constexpr uint8_t kPitchParamCount = 6;
constexpr EffectParamSpec kPitchParams[kPitchParamCount] = {
    {"input_gain", "Input Gain", 0.0f, 4.0f, 1.0f},
    {"semitones", "Semitones", -12.0f, 12.0f, 0.0f},
    {"mix", "Mix", 0.0f, 1.0f, 1.0f},
    {"output_gain", "Output Gain", 0.0f, 4.0f, 1.0f},
    {"noise_floor", "Noise Floor", 0.0001f, 0.02f, 0.0010f},
    {"wet_lpf_hz", "Wet LPF Hz", 1000.0f, 18000.0f, 12000.0f},
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

constexpr EffectTypeSpec kTypeSpecs[] = {
    {EFFECT_GAIN, kGainParams, kGainParamCount},
    {EFFECT_DISTORTION, kDistortionParams, kDistortionParamCount},
    {EFFECT_PITCH, kPitchParams, kPitchParamCount},
    {EFFECT_REVERB, kReverbParams, kReverbParamCount},
    {EFFECT_GATE, kGateParams, kGateParamCount},
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

ReverbState *getReverbStateFor(const char *effectId, uint8_t channel) {
    static std::unordered_map<std::string, ReverbState> states;
    std::string key = effectId ? effectId : "fx";
    key.push_back('#');
    key += std::to_string((unsigned)channel);
    return &states[key];
}

GateState *getGateStateFor(const char *effectId, uint8_t channel) {
    static std::unordered_map<std::string, GateState> states;
    std::string key = effectId ? effectId : "fx";
    key.push_back('#');
    key += std::to_string((unsigned)channel);

    auto it = states.find(key);
    if (it != states.end()) {
        return &it->second;
    }

    GateState state = {};
    state.gain = 1.0f;
    auto inserted = states.emplace(key, state);
    return &inserted.first->second;
}

} // namespace

uint8_t effectTypeFromString(const char *text) {
    if (!text) {
        return EFFECT_GAIN;
    }
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
    return EFFECT_GAIN;
}

const char *effectTypeToString(uint8_t type) {
    switch (type) {
        case EFFECT_DISTORTION:
            return "distortion";
        case EFFECT_PITCH:
            return "pitch";
        case EFFECT_REVERB:
            return "reverb";
        case EFFECT_GATE:
            return "gate";
        case EFFECT_GAIN:
        default:
            return "gain";
    }
}

const EffectTypeSpec *effectTypeSpecFor(uint8_t type) {
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
    if (!params || !paramName || !paramName[0]) {
        return RET_ERR;
    }

    uint8_t index = 0;
    const EffectParamSpec *spec = effectParamSpecFor(params->type, paramName, &index);
    if (!spec || index >= EFFECT_PARAM_MAX) {
        return RET_ERR;
    }

    params->values[index] = clampValue(value, spec->minValue, spec->maxValue);
    return RET_OK;
}

int setSlotParamNormalized(SlotParams *params, const char *paramName, float normalized) {
    if (!params || !paramName || !paramName[0]) {
        return RET_ERR;
    }

    uint8_t index = 0;
    const EffectParamSpec *spec = effectParamSpecFor(params->type, paramName, &index);
    if (!spec || index >= EFFECT_PARAM_MAX) {
        return RET_ERR;
    }

    normalized = clampValue(normalized, 0.0f, 1.0f);
    const float value = spec->minValue + (spec->maxValue - spec->minValue) * normalized;
    params->values[index] = value;
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

    *outValue = clampValue(params.values[index], spec->minValue, spec->maxValue);
    return RET_OK;
}

void setSlotDefaultsForType(SlotParams *params, uint8_t type) {
    if (!params) {
        return;
    }

    memset(params->values, 0, sizeof(params->values));
    const EffectTypeSpec *spec = effectTypeSpecFor(type);
    const uint8_t limit = (spec->paramCount > EFFECT_PARAM_MAX) ? EFFECT_PARAM_MAX : spec->paramCount;
    for (uint8_t i = 0; i < limit; ++i) {
        params->values[i] = spec->params[i].defaultValue;
    }
}

void clampSlotParams(SlotParams *params) {
    if (!params) {
        return;
    }

    params->enabled = params->enabled ? 1U : 0U;
    if (params->type != EFFECT_GAIN &&
        params->type != EFFECT_DISTORTION &&
        params->type != EFFECT_PITCH &&
        params->type != EFFECT_REVERB &&
        params->type != EFFECT_GATE) {
        params->type = EFFECT_GAIN;
    }

    const EffectTypeSpec *spec = effectTypeSpecFor(params->type);
    const uint8_t limit = (spec->paramCount > EFFECT_PARAM_MAX) ? EFFECT_PARAM_MAX : spec->paramCount;
    for (uint8_t i = 0; i < limit; ++i) {
        params->values[i] = clampValue(params->values[i], spec->params[i].minValue, spec->params[i].maxValue);
    }
    for (uint8_t i = limit; i < EFFECT_PARAM_MAX; ++i) {
        params->values[i] = 0.0f;
    }
}

void processSlot(const char *effectId,
                 uint8_t channel,
                 const SlotParams &params,
                 const float *in,
                 float *out,
                 uint32_t frames) {
    if (!in || !out) {
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
        case EFFECT_DISTORTION:
            processDistortion(in,
                              out,
                              frames,
                              clamped.values[0],
                              clamped.values[1],
                              clamped.values[2],
                              clamped.values[3]);
            return;
        case EFFECT_PITCH:
            processPitch(effectId,
                         channel,
                         in,
                         out,
                         frames,
                         clamped.values[0],
                         clamped.values[1],
                         clamped.values[2],
                         clamped.values[3],
                         clamped.values[4],
                         clamped.values[5]);
            return;
        case EFFECT_REVERB:
            processReverb(in,
                          out,
                          frames,
                          clamped.values[0],
                          clamped.values[1],
                          clamped.values[2],
                          clamped.values[3],
                          getReverbStateFor(effectId, channel));
            return;
        case EFFECT_GATE:
            processGate(in,
                        out,
                        frames,
                        clamped.values[0],
                        clamped.values[1],
                        clamped.values[2],
                        clamped.values[3],
                        getGateStateFor(effectId, channel));
            return;
        case EFFECT_GAIN:
        default:
            processGain(in, out, frames, clamped.values[0]);
            return;
    }
}

} // namespace audiox::effects
