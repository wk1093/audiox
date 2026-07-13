#pragma once

#include "defs.hpp"

#include <climits>
#include <errno.h>
#include <sound/asound.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#define AUDIO_INPUT_FALLBACK_RATE 48000U
#define AUDIO_CAPTURE_PERIOD_FRAMES 256U
#define AUDIO_CAPTURE_PERIODS 4U

static inline void audio_pcm_hw_params_any(struct snd_pcm_hw_params *params) {
    memset(params, 0, sizeof(*params));

    for (size_t maskIndex = 0; maskIndex < (sizeof(params->masks) / sizeof(params->masks[0])); ++maskIndex) {
        for (size_t bitIndex = 0; bitIndex < (sizeof(params->masks[maskIndex].bits) / sizeof(params->masks[maskIndex].bits[0])); ++bitIndex) {
            params->masks[maskIndex].bits[bitIndex] = 0xFFFFFFFFU;
        }
    }

    for (size_t intervalIndex = 0; intervalIndex < (sizeof(params->intervals) / sizeof(params->intervals[0])); ++intervalIndex) {
        params->intervals[intervalIndex].min = 0;
        params->intervals[intervalIndex].max = UINT_MAX;
        params->intervals[intervalIndex].openmin = 0;
        params->intervals[intervalIndex].openmax = 0;
        params->intervals[intervalIndex].integer = 0;
        params->intervals[intervalIndex].empty = 0;
    }
}

static inline void audio_pcm_hw_params_set_mask(struct snd_pcm_hw_params *params,
                                                int param,
                                                unsigned int value) {
    if (param < SNDRV_PCM_HW_PARAM_FIRST_MASK || param > SNDRV_PCM_HW_PARAM_LAST_MASK) {
        return;
    }

    struct snd_mask *mask = &params->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(mask, 0, sizeof(*mask));
    mask->bits[value / 32U] |= (1U << (value % 32U));
    params->rmask |= (1U << param);
}

static inline void audio_pcm_hw_params_set_interval(struct snd_pcm_hw_params *params,
                                                    int param,
                                                    unsigned int minValue,
                                                    unsigned int maxValue,
                                                    int integerOnly) {
    if (param < SNDRV_PCM_HW_PARAM_FIRST_INTERVAL || param > SNDRV_PCM_HW_PARAM_LAST_INTERVAL) {
        return;
    }

    struct snd_interval *interval = &params->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    interval->min = minValue;
    interval->max = maxValue;
    interval->openmin = 0;
    interval->openmax = 0;
    interval->integer = integerOnly ? 1 : 0;
    interval->empty = 0;
    params->rmask |= (1U << param);
}

static inline int audio_pcm_configure_hw_fd(int fd,
                                            const char *path,
                                            unsigned int rate,
                                            unsigned int channels,
                                            unsigned int periodFrames,
                                            unsigned int periods,
                                            snd_pcm_uframes_t *periodFramesOut,
                                            size_t *frameBytesOut,
                                            int configureTiming,
                                            int logFail) {
    struct snd_pcm_hw_params hw;
    audio_pcm_hw_params_any(&hw);

    const unsigned int bufferFrames = periodFrames * periods;

    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);

    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, channels, channels, 1);
    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_RATE, rate, rate, 1);
    if (configureTiming) {
        audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, periodFrames, periodFrames, 1);
        audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIODS, periods, periods, 1);
        audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, bufferFrames, bufferFrames, 1);
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        if (logFail) {
            printf("[INIT] [ERR] ALSA HW_PARAMS failed on %s: %s\n", path, strerror(errno));
        }
        return -1;
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE, nullptr) < 0) {
        if (logFail) {
            printf("[INIT] [ERR] ALSA PREPARE failed on %s: %s\n", path, strerror(errno));
        }
        return -1;
    }

    if (periodFramesOut) {
        *periodFramesOut = periodFrames;
    }
    if (frameBytesOut) {
        *frameBytesOut = sizeof(int16_t) * channels;
    }
    return 0;
}