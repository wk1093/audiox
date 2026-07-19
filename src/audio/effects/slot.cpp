#include "audio/effects/slot.hpp"

#include "audio/effects/distortion.hpp"
#include "audio/effects/gain.hpp"

#include <string.h>

namespace audiox::effects {

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
    return EFFECT_GAIN;
}

const char *effectTypeToString(uint8_t type) {
    switch (type) {
        case EFFECT_DISTORTION:
            return "distortion";
        case EFFECT_GAIN:
        default:
            return "gain";
    }
}

void clampSlotParams(SlotParams *params) {
    if (!params) {
        return;
    }

    params->enabled = params->enabled ? 1U : 0U;
    if (params->type != EFFECT_GAIN && params->type != EFFECT_DISTORTION) {
        params->type = EFFECT_GAIN;
    }

    if (params->gain < 0.0f) {
        params->gain = 0.0f;
    }
    if (params->gain > 4.0f) {
        params->gain = 4.0f;
    }

    if (params->drive < 0.0f) {
        params->drive = 0.0f;
    }
    if (params->drive > 8.0f) {
        params->drive = 8.0f;
    }

    if (params->clip < 0.05f) {
        params->clip = 0.05f;
    }
    if (params->clip > 1.0f) {
        params->clip = 1.0f;
    }

    if (params->output < 0.0f) {
        params->output = 0.0f;
    }
    if (params->output > 4.0f) {
        params->output = 4.0f;
    }
}

void processSlot(const SlotParams &params,
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

    switch (params.type) {
        case EFFECT_DISTORTION:
            processDistortion(in, out, frames, params.drive, params.clip, params.output);
            return;
        case EFFECT_GAIN:
        default:
            processGain(in, out, frames, params.gain);
            return;
    }
}

} // namespace audiox::effects
