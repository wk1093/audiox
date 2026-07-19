#include "audio/effects/slot.hpp"

#include "audio/effects/distortion.hpp"
#include "audio/effects/gain.hpp"
#include "audio/effects/pitch.hpp"
#include "audio/effects/reverb.hpp"

#include <string.h>
#include <string>
#include <unordered_map>

namespace audiox::effects {

namespace {

ReverbState *getReverbStateFor(const char *effectId, uint8_t channel) {
    static std::unordered_map<std::string, ReverbState> states;
    std::string key = effectId ? effectId : "fx";
    key.push_back('#');
    key += std::to_string((unsigned)channel);
    return &states[key];
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
    if (params->type != EFFECT_GAIN &&
        params->type != EFFECT_DISTORTION &&
        params->type != EFFECT_PITCH &&
        params->type != EFFECT_REVERB) {
        params->type = EFFECT_GAIN;
    }

    if (params->gain < 0.0f) {
        params->gain = 0.0f;
    }
    if (params->gain > 4.0f) {
        params->gain = 4.0f;
    }

    if (params->drive < -24.0f) {
        params->drive = -24.0f;
    }
    if (params->drive > 24.0f) {
        params->drive = 24.0f;
    }

    if (params->clip < 0.0f) {
        params->clip = 0.0f;
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

    switch (params.type) {
        case EFFECT_DISTORTION:
            processDistortion(in, out, frames, params.drive, params.clip, params.output);
            return;
        case EFFECT_PITCH:
            processPitch(effectId, channel, in, out, frames, params.gain, params.drive, params.clip, params.output);
            return;
        case EFFECT_REVERB:
            processReverb(in,
                          out,
                          frames,
                          params.gain,
                          params.drive,
                          params.clip,
                          params.output,
                          getReverbStateFor(effectId, channel));
            return;
        case EFFECT_GAIN:
        default:
            processGain(in, out, frames, params.gain);
            return;
    }
}

} // namespace audiox::effects
