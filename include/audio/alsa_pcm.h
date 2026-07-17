#pragma once

#include "defs.hpp"

#include <alsa/asoundlib.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define AUDIO_INPUT_FALLBACK_RATE 48000U
#define AUDIO_CAPTURE_PERIOD_FRAMES 256U
#define AUDIO_CAPTURE_PERIODS 4U

static inline int audio_pcm_configure_hw_handle(snd_pcm_t *pcm,
                                                const char *path,
                                                unsigned int rate,
                                                unsigned int channels,
                                                snd_pcm_format_t format,
                                                unsigned int periodFrames,
                                                unsigned int periods,
                                                snd_pcm_uframes_t *periodFramesOut,
                                                size_t *frameBytesOut,
                                                int configureTiming,
                                                int logFail) {
    if (!pcm) {
        return -EINVAL;
    }

    snd_pcm_hw_params_t *hw = nullptr;
    int rc = snd_pcm_hw_params_malloc(&hw);
    if (rc < 0) {
        if (logFail) {
            printf("[INIT] [ERR] ALSA HW_PARAMS alloc failed on %s: %s\n",
                   path,
                   snd_strerror(rc));
        }
        return rc;
    }

    const snd_pcm_uframes_t requestedPeriodFrames = (snd_pcm_uframes_t)periodFrames;
    const unsigned int requestedPeriods = periods;
    const snd_pcm_uframes_t requestedBufferFrames = requestedPeriodFrames * (snd_pcm_uframes_t)requestedPeriods;

    rc = snd_pcm_hw_params_any(pcm, hw);
    if (rc >= 0) {
        rc = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    }
    if (rc >= 0) {
        rc = snd_pcm_hw_params_set_format(pcm, hw, format);
    }
    if (rc >= 0) {
        rc = snd_pcm_hw_params_set_channels(pcm, hw, channels);
    }
    if (rc >= 0) {
        unsigned int exactRate = rate;
        rc = snd_pcm_hw_params_set_rate_near(pcm, hw, &exactRate, nullptr);
    }

    if (rc >= 0 && configureTiming) {
        snd_pcm_uframes_t exactPeriodFrames = requestedPeriodFrames;
        unsigned int exactPeriods = requestedPeriods;
        snd_pcm_uframes_t exactBufferFrames = requestedBufferFrames;

        rc = snd_pcm_hw_params_set_period_size_near(pcm, hw, &exactPeriodFrames, nullptr);
        if (rc >= 0) {
            rc = snd_pcm_hw_params_set_periods_near(pcm, hw, &exactPeriods, nullptr);
        }
        if (rc >= 0) {
            rc = snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &exactBufferFrames);
        }
    }

    if (rc >= 0) {
        rc = snd_pcm_hw_params(pcm, hw);
    }

    snd_pcm_uframes_t actualPeriodFrames = requestedPeriodFrames;
    if (rc >= 0) {
        rc = snd_pcm_hw_params_get_period_size(hw, &actualPeriodFrames, nullptr);
    }

    snd_pcm_hw_params_free(hw);

    if (rc < 0) {
        if (logFail) {
            printf("[INIT] [ERR] ALSA HW_PARAMS failed on %s: %s\n", path, snd_strerror(rc));
        }
        return rc;
    }

    rc = snd_pcm_prepare(pcm);
    if (rc < 0) {
        if (logFail) {
            printf("[INIT] [ERR] ALSA PREPARE failed on %s: %s\n", path, snd_strerror(rc));
        }
        return rc;
    }

    snd_pcm_sw_params_t *sw = nullptr;
    rc = snd_pcm_sw_params_malloc(&sw);
    if (rc >= 0) {
        rc = snd_pcm_sw_params_current(pcm, sw);
    }
    if (rc >= 0) {
        rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, 1U);
    }
    if (rc >= 0) {
        snd_pcm_uframes_t availMin = (actualPeriodFrames > 0) ? actualPeriodFrames : 1U;
        rc = snd_pcm_sw_params_set_avail_min(pcm, sw, availMin);
    }
    if (rc >= 0) {
        rc = snd_pcm_sw_params(pcm, sw);
    }
    if (sw) {
        snd_pcm_sw_params_free(sw);
    }
    if (rc < 0) {
        if (logFail) {
            printf("[INIT] [ERR] ALSA SW_PARAMS failed on %s: %s\n", path, snd_strerror(rc));
        }
        return rc;
    }

    if (periodFramesOut) {
        *periodFramesOut = actualPeriodFrames;
    }
    if (frameBytesOut) {
        int widthBits = snd_pcm_format_physical_width(format);
        size_t widthBytes = (widthBits > 0) ? (size_t)((widthBits + 7) / 8) : sizeof(int16_t);
        *frameBytesOut = widthBytes * channels;
    }
    return 0;
}

static inline int audio_pcm_open_configured(snd_pcm_t **pcmOut,
                                            const char *name,
                                            snd_pcm_stream_t stream,
                                            unsigned int rate,
                                            unsigned int channels,
                                            snd_pcm_format_t format,
                                            unsigned int periodFrames,
                                            unsigned int periods,
                                            snd_pcm_uframes_t *periodFramesOut,
                                            size_t *frameBytesOut,
                                            int configureTiming,
                                            int logFail) {
    if (!pcmOut || !name) {
        return -EINVAL;
    }

    *pcmOut = nullptr;
    snd_pcm_t *pcm = nullptr;
    int rc = snd_pcm_open(&pcm, name, stream, SND_PCM_NONBLOCK);
    if (rc < 0) {
        if (logFail) {
            printf("[INIT] [ERR] ALSA open failed on %s: %s\n", name, snd_strerror(rc));
        }
        return rc;
    }

    rc = audio_pcm_configure_hw_handle(pcm,
                                       name,
                                       rate,
                                       channels,
                                       format,
                                       periodFrames,
                                       periods,
                                       periodFramesOut,
                                       frameBytesOut,
                                       configureTiming,
                                       logFail);
    if (rc < 0) {
        snd_pcm_close(pcm);
        return rc;
    }

    *pcmOut = pcm;
    return 0;
}

static inline int audio_pcm_recover(snd_pcm_t *pcm, int err, const char *path, const char *op) {
    if (!pcm) {
        return -EINVAL;
    }

    int rc = snd_pcm_recover(pcm, err, 0);
    if (rc < 0) {
        printf("[AUDIO] [WARN] ALSA %s recover failed on %s: %s\n",
               op ? op : "stream",
               path ? path : "(unknown)",
               snd_strerror(rc));
    }
    return rc;
}