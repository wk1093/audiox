#pragma once

#include <cstdint>

namespace audiox::effects {

enum EffectType : uint8_t {
    EFFECT_GAIN = 0,
    EFFECT_DISTORTION = 1,
};

struct SlotParams {
    uint8_t enabled;
    uint8_t type;
    float gain;
    float drive;
    float clip;
    float output;
};

uint8_t effectTypeFromString(const char *text);
const char *effectTypeToString(uint8_t type);
void clampSlotParams(SlotParams *params);
void processSlot(const SlotParams &params,
                 const float *in,
                 float *out,
                 uint32_t frames);

} // namespace audiox::effects
