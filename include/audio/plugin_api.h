#ifndef AUDIOX_PLUGIN_API_H
#define AUDIOX_PLUGIN_API_H

#include <stdint.h>

#define AUDIOX_PLUGIN_ABI_VERSION 1u
#define AUDIOX_PLUGIN_MAX_PARAMS 32u
#define AUDIOX_PLUGIN_ID_MAX 23u
#define AUDIOX_PLUGIN_PARAM_NAME_MAX 23u
#define AUDIOX_PLUGIN_LABEL_MAX 63u

#if defined(__GNUC__)
#define AUDIOX_PLUGIN_EXPORT __attribute__((visibility("default")))
#else
#define AUDIOX_PLUGIN_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AudioxParameterV1 {
    const char *name;
    const char *label;
    float min;
    float max;
    float default_value;
} AudioxParameterV1;

typedef struct AudioxPluginV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const char *id;
    uint32_t param_count;
    const AudioxParameterV1 *params;
    void *(*create)(uint32_t sample_rate, uint32_t max_frames);
    void (*destroy)(void *instance);
    void (*reset)(void *instance);
    void (*process)(void *instance, const float *input, float *output,
                    uint32_t frames, const float *params);
    int (*tail)(void *instance, float threshold);
} AudioxPluginV1;

typedef const AudioxPluginV1 *(*AudioxPluginEntryV1)(void);
AUDIOX_PLUGIN_EXPORT const AudioxPluginV1 *audiox_plugin_v1(void);

#ifdef __cplusplus
}
#endif
#endif
