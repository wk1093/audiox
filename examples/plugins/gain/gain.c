#include "audio/plugin_api.h"
#include <stdlib.h>

typedef struct { uint32_t max_frames; } Gain;
static void *gain_create(uint32_t sample_rate, uint32_t max_frames) {
    if (!sample_rate || !max_frames) return NULL;
    Gain *gain = (Gain *)malloc(sizeof(*gain));
    if (gain) gain->max_frames = max_frames;
    return gain;
}
static void gain_destroy(void *instance) { free(instance); }
static void gain_reset(void *instance) { (void)instance; }
static void gain_process(void *instance, const float *input, float *output,
                         uint32_t frames, const float *params) {
    const Gain *gain = (const Gain *)instance;
    if (frames > gain->max_frames) return;
    for (uint32_t i = 0; i < frames; ++i) output[i] = input[i] * params[0];
}
static int gain_tail(void *instance, float threshold) {
    (void)instance; (void)threshold; return 0;
}
static const AudioxParameterV1 parameters[] = {{"gain", "Gain", 0.0f, 2.0f, 1.0f}};
static const AudioxPluginV1 descriptor = {
    AUDIOX_PLUGIN_ABI_VERSION, sizeof(AudioxPluginV1), "plugin_gain", 1, parameters,
    gain_create, gain_destroy, gain_reset, gain_process, gain_tail
};
AUDIOX_PLUGIN_EXPORT const AudioxPluginV1 *audiox_plugin_v1(void) { return &descriptor; }
