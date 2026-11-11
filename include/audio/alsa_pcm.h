#pragma once

#include "defs.hpp"
#include "audio/health.hpp"

#include <alsa/asoundlib.h>
#include <atomic>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#define AUDIO_INPUT_FALLBACK_RATE 48000U
#define AUDIO_CAPTURE_PERIOD_FRAMES 256U
#define AUDIO_CAPTURE_PERIODS 4U

static inline int audio_pcm_should_log_underrun_count(uint64_t count) {
    if (count == 1U || count == 10U || count == 50U || count == 100U) {
        return 1;
    }
    if (count > 100U && (count % 250U) == 0U) {
        return 1;
    }
    return 0;
}

static inline void audiox_alsa_error_handler(const char *file,
                                             int line,
                                             const char *function,
                                             int err,
                                             const char *fmt,
                                             ...) {
    char msg[256];
    msg[0] = '\0';

    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(msg, sizeof(msg), fmt ? fmt : "", ap);
    va_end(ap);

#if !AUDIOX_ENABLE_ALSA_LOGS
    // Suppress only the noisy recover-underrun spam line.
    if (err == -EPIPE &&
        function && strstr(function, "snd_pcm_recover") != nullptr &&
        strstr(msg, "underrun occurred") != nullptr) {
        return;
    }
#endif

    // The stream probe code intentionally tries many candidate formats/rates,
    // so native hw_open failures become pure retry noise.
    if (function && strstr(function, "snd_pcm_hw_open") != nullptr &&
        msg[0] != '\0' && strstr(msg, "open '") != nullptr &&
        strstr(msg, "failed (") != nullptr) {
        return;
    }

    fprintf(stderr,
            "ALSA lib %s:%d:(%s) %s\n",
            file ? file : "(unknown)",
            line,
            function ? function : "(unknown)",
            msg[0] ? msg : "(no message)");
}

static inline void audio_pcm_install_error_handler() {
    static int installed = 0;
    if (installed) {
        return;
    }
    snd_lib_error_set_handler(audiox_alsa_error_handler);
    installed = 1;
}

static inline int audio_pcm_close(snd_pcm_t *pcm, const char *path, const char *op) {
    const snd_pcm_state_t state = snd_pcm_state(pcm);
    const int rc = snd_pcm_close(pcm);
    if (rc < 0) {
        printf("[AUDIO] [WARN] ALSA %s close failed on %s (state=%s, error=%d): %s\n",
               op ? op : "stream", path ? path : "(unknown)",
               snd_pcm_state_name(state), rc, snd_strerror(rc));
    }
    return rc;
}

static inline void audio_pcm_track_underrun(const char *path, const char *op) {
    audiox::audioHealth.recordXrun();
    audiox::adaptiveAudioBuffer.recordXrun(audiox::healthMonotonicMs(), BUFFER_FRAMES);
    static std::atomic<uint64_t> underrunCount{0};
    uint64_t count = underrunCount.fetch_add(1U, std::memory_order_relaxed) + 1U;
    if (!audio_pcm_should_log_underrun_count(count)) {
        return;
    }

    printf("[AUDIO] [WARN] ALSA underrun count=%llu (%s on %s)\n",
           (unsigned long long)count,
           op ? op : "stream",
           path ? path : "(unknown)");
}

static inline snd_pcm_uframes_t audio_pcm_playback_start_frames(
    snd_pcm_uframes_t periodFrames, snd_pcm_uframes_t bufferFrames) {
    // Keep a real scheduling/drift reserve even at the smallest adaptive tier.
    const snd_pcm_uframes_t target =
        std::max(periodFrames, bufferFrames > periodFrames ? bufferFrames - periodFrames : 0U);
    return std::min(target, bufferFrames);
}

static inline int audio_pcm_start_playback_if_ready(snd_pcm_t *pcm,
                                                   snd_pcm_uframes_t periodFrames,
                                                   snd_pcm_uframes_t bufferFrames,
                                                   const char *path) {
    if (snd_pcm_state(pcm) != SND_PCM_STATE_PREPARED) return 0;
    const snd_pcm_sframes_t available = snd_pcm_avail_update(pcm);
    int rc = 0;
    if (available < 0) {
        rc = static_cast<int>(available);
    } else if (static_cast<snd_pcm_uframes_t>(available) <= bufferFrames &&
               bufferFrames - static_cast<snd_pcm_uframes_t>(available) >=
                   audio_pcm_playback_start_frames(periodFrames, bufferFrames)) {
        rc = snd_pcm_start(pcm);
    }
    if (rc < 0) {
        printf("[AUDIO] [WARN] ALSA playback start failed on %s: %s\n",
               path ? path : "(unknown)", snd_strerror(rc));
    }
    return rc;
}

static inline int audio_pcm_configure_hw_handle(snd_pcm_t *pcm,
                                                const char *path,
                                                unsigned int rate,
                                                unsigned int channels,
                                                snd_pcm_format_t format,
                                                snd_pcm_access_t access,
                                                unsigned int periodFrames,
                                                unsigned int periods,
                                                snd_pcm_uframes_t *periodFramesOut,
                                                snd_pcm_uframes_t *bufferFramesOut,
                                                size_t *frameBytesOut,
                                                int configureTiming,
                                                int logFail,
                                                unsigned int *rateOut = nullptr) {
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
        rc = snd_pcm_hw_params_set_access(pcm, hw, access);
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
    snd_pcm_uframes_t actualBufferFrames = requestedBufferFrames;
    if (rc >= 0) {
        rc = snd_pcm_hw_params_get_period_size(hw, &actualPeriodFrames, nullptr);
    }
    if (rc >= 0) {
        rc = snd_pcm_hw_params_get_buffer_size(hw, &actualBufferFrames);
    }
    unsigned int actualRate = rate;
    if (rc >= 0 && rateOut) rc = snd_pcm_hw_params_get_rate(hw, &actualRate, nullptr);

    snd_pcm_hw_params_free(hw);

    if (rc < 0) {
        if (logFail) {
            printf("[INIT] [ERR] ALSA HW_PARAMS failed on %s: %s\n", path, snd_strerror(rc));
        }
        return rc;
    }
    if (rateOut) *rateOut = actualRate;

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
        const snd_pcm_uframes_t startFrames = snd_pcm_stream(pcm) == SND_PCM_STREAM_PLAYBACK
            ? audio_pcm_playback_start_frames(actualPeriodFrames, actualBufferFrames) : 1U;
        rc = snd_pcm_sw_params_set_start_threshold(pcm, sw, startFrames);
    }
    if (rc >= 0) {
        snd_pcm_uframes_t availMin = actualPeriodFrames > 0
            ? std::min<snd_pcm_uframes_t>(actualPeriodFrames, BUFFER_FRAMES) : 1U;
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
    if (bufferFramesOut) {
        *bufferFramesOut = actualBufferFrames;
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
                                            snd_pcm_access_t *access,
                                            unsigned int periodFrames,
                                            unsigned int periods,
                                            snd_pcm_uframes_t *periodFramesOut,
                                            snd_pcm_uframes_t *bufferFramesOut,
                                            size_t *frameBytesOut,
                                            int configureTiming,
                                            int logFail,
                                            unsigned int *rateOut = nullptr) {
    if (!pcmOut || !name || !access) {
        return -EINVAL;
    }

    *pcmOut = nullptr;
    const snd_pcm_access_t requestedAccess = *access;
    const snd_pcm_access_t alternatives[] = {
        requestedAccess,
        (requestedAccess == SND_PCM_ACCESS_MMAP_INTERLEAVED)
            ? SND_PCM_ACCESS_RW_INTERLEAVED
            : SND_PCM_ACCESS_MMAP_INTERLEAVED,
    };

    int rc = -EINVAL;
    for (size_t i = 0; i < (sizeof(alternatives) / sizeof(alternatives[0])); ++i) {
        snd_pcm_t *pcm = nullptr;
        rc = snd_pcm_open(&pcm, name, stream, SND_PCM_NONBLOCK);
        if (rc < 0) {
            continue;
        }

        rc = audio_pcm_configure_hw_handle(pcm,
                                           name,
                                           rate,
                                           channels,
                                           format,
                                           alternatives[i],
                                           periodFrames,
                                           periods,
                                           periodFramesOut,
                                           bufferFramesOut,
                                           frameBytesOut,
                                           configureTiming,
                                           logFail,
                                           rateOut);
        if (rc >= 0) {
            *pcmOut = pcm;
            *access = alternatives[i];
            return 0;
        }
        audio_pcm_close(pcm, name, stream == SND_PCM_STREAM_PLAYBACK ? "playback probe" : "capture probe");
    }

    if (logFail) {
        printf("[INIT] [ERR] ALSA open/configure failed on %s: %s\n", name, snd_strerror(rc));
    }
    return rc;
}

static inline int audio_pcm_recover(snd_pcm_t *pcm, int err, const char *path, const char *op) {
    if (!pcm) {
        return -EINVAL;
    }

    if (err == -EPIPE) {
        audio_pcm_track_underrun(path, op);
    }

    int rc = snd_pcm_recover(pcm, err, 1);
    if (rc < 0) {
        printf("[AUDIO] [WARN] ALSA %s recover failed on %s: %s\n",
               op ? op : "stream",
               path ? path : "(unknown)",
               snd_strerror(rc));
    }
    return rc;
}

static inline int audio_pcm_recover_capture(snd_pcm_t *pcm, int err,
                                            const char *path) {
    int rc = audio_pcm_recover(pcm, err, path, "capture");
    if (rc < 0) return rc;

    if (snd_pcm_state(pcm) == SND_PCM_STATE_PREPARED) {
        rc = snd_pcm_start(pcm);
        if (rc < 0) {
            printf("[AUDIO] [WARN] ALSA capture restart failed on %s: %s\n",
                   path ? path : "(unknown)", snd_strerror(rc));
        }
    }
    return rc;
}

static inline int audio_pcm_recover_playback(snd_pcm_t *pcm, int err,
                                            const char *path) {
    // ALSA's suspend recovery retries resume with sleeps; re-prepare instead on the RT path.
    if (err == -ESTRPIPE) return pcm ? snd_pcm_prepare(pcm) : -EINVAL;
    return audio_pcm_recover(pcm, err, path, "playback");
}