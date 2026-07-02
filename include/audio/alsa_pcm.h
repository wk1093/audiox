#ifndef AUDIO_ALSA_PCM_H
#define AUDIO_ALSA_PCM_H

#include "audio/types.h"

static inline void audio_pcm_hw_params_any(struct snd_pcm_hw_params *params) {
    memset(params, 0, sizeof(*params));

    for (size_t m = 0; m < (sizeof(params->masks) / sizeof(params->masks[0])); ++m) {
        for (size_t b = 0; b < (sizeof(params->masks[m].bits) / sizeof(params->masks[m].bits[0])); ++b) {
            params->masks[m].bits[b] = 0xFFFFFFFFU;
        }
    }

    for (size_t i = 0; i < (sizeof(params->intervals) / sizeof(params->intervals[0])); ++i) {
        params->intervals[i].min = 0;
        params->intervals[i].max = UINT_MAX;
        params->intervals[i].openmin = 0;
        params->intervals[i].openmax = 0;
        params->intervals[i].integer = 0;
        params->intervals[i].empty = 0;
    }
}

static inline void audio_pcm_hw_params_set_mask(struct snd_pcm_hw_params *params, int param, unsigned int value) {
    if (param < SNDRV_PCM_HW_PARAM_FIRST_MASK || param > SNDRV_PCM_HW_PARAM_LAST_MASK) {
        return;
    }

    struct snd_mask *mask = &params->masks[param - SNDRV_PCM_HW_PARAM_FIRST_MASK];
    memset(mask, 0, sizeof(*mask));
    mask->bits[value / 32U] |= (1U << (value % 32U));
    params->rmask |= (1U << param);
}

static inline void audio_pcm_hw_params_set_interval(
    struct snd_pcm_hw_params *params,
    int param,
    unsigned int min_value,
    unsigned int max_value,
    int integer_only) {
    if (param < SNDRV_PCM_HW_PARAM_FIRST_INTERVAL || param > SNDRV_PCM_HW_PARAM_LAST_INTERVAL) {
        return;
    }

    struct snd_interval *ival = &params->intervals[param - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL];
    ival->min = min_value;
    ival->max = max_value;
    ival->openmin = 0;
    ival->openmax = 0;
    ival->integer = integer_only ? 1 : 0;
    ival->empty = 0;
    params->rmask |= (1U << param);
}

static inline int audio_pcm_configure_hw(audio_ctx_t *ctx) {
    struct snd_pcm_hw_params hw;
    audio_pcm_hw_params_any(&hw);

    const unsigned int channels = 2;
    const unsigned int rate = SAMPLE_RATE;
    const unsigned int period_frames = BUFFER_FRAMES;
    const unsigned int periods = 4;
    const unsigned int buffer_frames = period_frames * periods;

    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);

    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, channels, channels, 1);
    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_RATE, rate, rate, 1);
    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period_frames, period_frames, 1);
    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIODS, periods, periods, 1);
    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, buffer_frames, buffer_frames, 1);

    if (ioctl(ctx->fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        printf("[INIT] [ERR] ALSA HW_PARAMS failed on %s: %s\n", ctx->pcm_path, strerror(errno));
        return -1;
    }

    if (ioctl(ctx->fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        printf("[INIT] [ERR] ALSA PREPARE failed on %s: %s\n", ctx->pcm_path, strerror(errno));
        return -1;
    }

    ctx->period_frames = period_frames;
    ctx->frame_bytes = sizeof(int16_t) * channels;
    return 0;
}

static inline int audio_open_pcm_device(audio_ctx_t *ctx) {
    const char *candidates[] = {
        "/dev/snd/pcmC0D0p",
        "/dev/snd/pcmC1D0p",
        "/dev/snd/pcmC2D0p",
        "/dev/snd/pcmC3D0p"
    };

    for (size_t i = 0; i < (sizeof(candidates) / sizeof(candidates[0])); ++i) {
        int fd = open(candidates[i], O_WRONLY);
        if (fd < 0) {
            continue;
        }

        ctx->fd = fd;
        snprintf(ctx->pcm_path, sizeof(ctx->pcm_path), "%s", candidates[i]);
        if (audio_pcm_configure_hw(ctx) == 0) {
            printf("[INIT] ALSA PCM opened: %s (S16_LE, 2ch, %d Hz, period=%u)\n",
                   ctx->pcm_path,
                   SAMPLE_RATE,
                   (unsigned)ctx->period_frames);
            return 0;
        }

        close(ctx->fd);
        ctx->fd = -1;
    }

    printf("[INIT] [ERR] No usable ALSA playback PCM found at /dev/snd/pcmC*D0p\n");
    return -1;
}

#endif
