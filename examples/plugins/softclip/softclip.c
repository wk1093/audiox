#include "audio/plugin_api.h"
#include <stdlib.h>

static void *clip_create(uint32_t sample_rate, uint32_t max_frames) {
    if (!sample_rate || !max_frames) return NULL;
    uint32_t *frames = (uint32_t *)malloc(sizeof(*frames));
    if (frames) *frames = max_frames;
    return frames;
}
static void clip_destroy(void *instance) { free(instance); }
static void clip_reset(void *instance) { (void)instance; }
static void clip_process(void *instance, const float *input, float *output,
                         uint32_t frames, const float *params) {
    if (frames > *(const uint32_t *)instance) return;
    for (uint32_t i = 0; i < frames; ++i) {
        float x = input[i] * params[0];
        float magnitude = x < 0.0f ? -x : x;
        output[i] = x / (1.0f + magnitude);
    }
}
static int clip_tail(void *instance, float threshold) {
    (void)instance; (void)threshold; return 0;
}
static const AudioxParameterV1 parameters[] = {{"drive", "Drive", 0.1f, 10.0f, 1.0f}};
static const AudioxPluginV1 descriptor = {
    AUDIOX_PLUGIN_ABI_VERSION, sizeof(AudioxPluginV1), "plugin_softclip", 1, parameters,
    clip_create, clip_destroy, clip_reset, clip_process, clip_tail
};
AUDIOX_PLUGIN_EXPORT const AudioxPluginV1 *audiox_plugin_v1(void) { return &descriptor; }
