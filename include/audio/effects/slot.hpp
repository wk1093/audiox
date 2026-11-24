#pragma once

#include <cstdint>

namespace audiox::plugin {
class PluginHost;
}

namespace audiox::effects {

// Advanced EQ: preamp, band count, and four parameters for each of 32 bands.
constexpr uint8_t EFFECT_PARAM_MAX = 130;

struct EffectParamSpec {
    const char *name;
    const char *label;
    float minValue;
    float maxValue;
    float defaultValue;
};

struct EffectTypeSpec {
    uint8_t type;
    const EffectParamSpec *params;
    uint8_t paramCount;
};

enum EffectType : uint8_t {
    EFFECT_GAIN = 0,
    EFFECT_DISTORTION = 1,
    EFFECT_PITCH = 2,
    EFFECT_REVERB = 3,
    EFFECT_GATE = 4,
    EFFECT_CUT = 5,
    EFFECT_COMPRESSOR = 6,
    EFFECT_LIMITER = 7,
    EFFECT_EQ = 8,
    EFFECT_MULTIBAND = 9,
    EFFECT_NOISE_REDUCTION = 10,
    EFFECT_ADVANCED_EQ = 11,
    EFFECT_BASS_ENHANCER = 12,
    EFFECT_CROSSFEED = 13,
    EFFECT_MAXIMIZER = 14,
    EFFECT_BUILTIN_LAST = EFFECT_MAXIMIZER,
};
bool isBuiltinEffectType(uint8_t type);
// Crossfeed and Maximizer process channels 0 and 1 together with state kept on channel 0.
inline bool isStereoLinkedType(uint8_t type) {
    return type == EFFECT_CROSSFEED || type == EFFECT_MAXIMIZER;
}
bool isIntegerEffectParam(uint8_t type, uint8_t index);

struct SlotParams {
    uint8_t enabled;
    uint8_t type;
    float values[EFFECT_PARAM_MAX];
    audiox::plugin::PluginHost *pluginHost{nullptr};
};

uint8_t effectTypeFromString(const char *text);
const char *effectTypeToString(uint8_t type);

const EffectTypeSpec *effectTypeSpecFor(uint8_t type);
const EffectParamSpec *effectParamSpecFor(uint8_t type, const char *paramName,
                                          uint8_t *outIndex = nullptr);
int setSlotParamValue(SlotParams *params, const char *paramName, float value);
int setSlotParamNormalized(SlotParams *params, const char *paramName, float normalized);
int getSlotParamValue(const SlotParams &params, const char *paramName, float *outValue);
void setSlotDefaultsForType(SlotParams *params, uint8_t type);
void clampSlotParams(SlotParams *params);
bool slotHasPendingAudio(const char *effectId, uint8_t channel, const SlotParams &params,
                         float threshold = 1.0e-12f);
bool slotNeedsSilentProcessing(const char *effectId, uint8_t channel, const SlotParams &params);
void resetSlotAudio(const char *effectId, uint8_t channel);
void prepareSlotAudio(const char *effectId, uint8_t channel, const SlotParams &params);
void processSlot(const char *effectId, uint8_t channel, const SlotParams &params, const float *in,
                 float *out, uint32_t frames);
// Stereo-linked types only; processSlot handles them as mono (L == R) nodes.
void processStereoSlot(const char *effectId, const SlotParams &params, const float *const in[2],
                       float *const out[2], uint32_t frames);

} // namespace audiox::effects
