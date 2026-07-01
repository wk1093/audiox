#ifndef AUDIO_OSS_H
#define AUDIO_OSS_H

#include <limits.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sound/asound.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "init_defs.h"

typedef struct audio_ctx {
    int fd;
    int enabled;
    pthread_mutex_t state_lock;
    char pcm_path[64];
    snd_pcm_uframes_t period_frames;
    size_t frame_bytes;
    uint8_t voice_enabled[4];
    uint16_t voice_gain_q15[4];
    struct wav_voice {
        int16_t *pcm;
        size_t frames;
        uint32_t sample_rate;
        uint16_t channels;
    } wav[4];
    uint32_t voice_cursor_q16[4];
    int16_t block_buffer[BUFFER_FRAMES * 2];
    size_t pending_offset;
    size_t pending_bytes;
    int warm_up_ticks;
    int waiting_for_host;
    uint32_t wait_notice_ticks;
} audio_ctx_t;

static inline size_t audio_block_bytes(void) {
    return sizeof(int16_t) * BUFFER_FRAMES * 2;
}

static inline int audio_voice_count(void) {
    return 4;
}

static inline const char *audio_voice_wav_path(int voice_index) {
    static const char *paths[4] = {
        "/etc/wavs/voice0.wav",
        "/etc/wavs/voice1.wav",
        "/etc/wavs/voice2.wav",
        "/etc/wavs/voice3.wav"
    };
    if (voice_index < 0 || voice_index >= (int)(sizeof(paths) / sizeof(paths[0]))) {
        return NULL;
    }
    return paths[voice_index];
}

static inline uint16_t audio_read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t audio_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline int audio_read_full(int fd, void *buf, size_t bytes) {
    uint8_t *p = (uint8_t *)buf;
    size_t done = 0;
    while (done < bytes) {
        ssize_t n = read(fd, p + done, bytes - done);
        if (n <= 0) {
            return -1;
        }
        done += (size_t)n;
    }
    return 0;
}

static inline void audio_unload_wav_voice(audio_ctx_t *ctx, int voice_index) {
    if (voice_index < 0 || voice_index >= audio_voice_count()) {
        return;
    }
    free(ctx->wav[voice_index].pcm);
    ctx->wav[voice_index].pcm = NULL;
    ctx->wav[voice_index].frames = 0;
    ctx->wav[voice_index].sample_rate = 0;
    ctx->wav[voice_index].channels = 0;
    ctx->voice_cursor_q16[voice_index] = 0;
}

static inline int audio_load_wav_voice(audio_ctx_t *ctx, int voice_index, const char *path) {
    uint8_t riff_header[12];
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[INIT] [ERR] Missing WAV for voice %d at %s\n", voice_index, path);
        return -1;
    }

    if (audio_read_full(fd, riff_header, sizeof(riff_header)) < 0 ||
        memcmp(riff_header, "RIFF", 4) != 0 ||
        memcmp(riff_header + 8, "WAVE", 4) != 0) {
        printf("[INIT] [ERR] Invalid WAV header: %s\n", path);
        close(fd);
        return -1;
    }

    uint16_t format = 0;
    uint16_t channels = 0;
    uint16_t bits_per_sample = 0;
    uint32_t sample_rate = 0;
    int got_fmt = 0;
    int got_data = 0;
    int16_t *pcm = NULL;
    size_t frames = 0;

    while (!got_data) {
        uint8_t chunk_header[8];
        if (audio_read_full(fd, chunk_header, sizeof(chunk_header)) < 0) {
            break;
        }

        uint32_t chunk_size = audio_read_le32(chunk_header + 4);
        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            uint8_t fmt[40];
            size_t to_read = chunk_size;
            if (to_read > sizeof(fmt)) {
                to_read = sizeof(fmt);
            }

            if (audio_read_full(fd, fmt, to_read) < 0) {
                break;
            }

            if (chunk_size > to_read) {
                size_t remaining = chunk_size - to_read;
                char sink[128];
                while (remaining > 0) {
                    size_t step = remaining < sizeof(sink) ? remaining : sizeof(sink);
                    if (audio_read_full(fd, sink, step) < 0) {
                        remaining = 0;
                        break;
                    }
                    remaining -= step;
                }
            }

            if (to_read < 16) {
                printf("[INIT] [ERR] WAV fmt chunk too small: %s\n", path);
                break;
            }

            format = audio_read_le16(fmt + 0);
            channels = audio_read_le16(fmt + 2);
            sample_rate = audio_read_le32(fmt + 4);
            bits_per_sample = audio_read_le16(fmt + 14);
            got_fmt = 1;
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            if (!got_fmt) {
                printf("[INIT] [ERR] WAV data chunk found before fmt chunk: %s\n", path);
                break;
            }

            if (format != 1 || (channels != 1 && channels != 2) || bits_per_sample != 16 || sample_rate == 0) {
                printf("[INIT] [ERR] Unsupported WAV format (%u ch=%u bits=%u rate=%u): %s\n",
                       (unsigned)format,
                       (unsigned)channels,
                       (unsigned)bits_per_sample,
                       (unsigned)sample_rate,
                       path);
                break;
            }

            size_t sample_bytes = chunk_size;
            if (sample_bytes < (size_t)channels * sizeof(int16_t)) {
                printf("[INIT] [ERR] WAV data chunk too small: %s\n", path);
                break;
            }

            pcm = (int16_t *)malloc(sample_bytes);
            if (!pcm) {
                printf("[INIT] [ERR] Out of memory while loading %s\n", path);
                break;
            }

            if (audio_read_full(fd, pcm, sample_bytes) < 0) {
                printf("[INIT] [ERR] Failed reading WAV PCM payload: %s\n", path);
                break;
            }

            frames = sample_bytes / ((size_t)channels * sizeof(int16_t));
            got_data = 1;
        } else {
            char sink[128];
            size_t remaining = chunk_size;
            while (remaining > 0) {
                size_t step = remaining < sizeof(sink) ? remaining : sizeof(sink);
                if (audio_read_full(fd, sink, step) < 0) {
                    remaining = 0;
                    break;
                }
                remaining -= step;
            }
        }

        if (chunk_size & 1U) {
            uint8_t pad = 0;
            if (audio_read_full(fd, &pad, 1) < 0) {
                break;
            }
        }
    }

    close(fd);

    if (!got_data || !pcm || frames == 0) {
        free(pcm);
        return -1;
    }

    audio_unload_wav_voice(ctx, voice_index);
    ctx->wav[voice_index].pcm = pcm;
    ctx->wav[voice_index].frames = frames;
    ctx->wav[voice_index].sample_rate = sample_rate;
    ctx->wav[voice_index].channels = channels;
    ctx->voice_cursor_q16[voice_index] = 0;

    printf("[INIT] Loaded voice %d WAV: %s (%u Hz, %u ch, %zu frames)\n",
           voice_index,
           path,
           (unsigned)sample_rate,
           (unsigned)channels,
           frames);
    return 0;
}

static inline int16_t audio_voice_next_sample(audio_ctx_t *ctx, int voice_index) {
    if (voice_index < 0 || voice_index >= audio_voice_count()) {
        return 0;
    }

    struct wav_voice *wv = &ctx->wav[voice_index];
    if (!wv->pcm || wv->frames == 0 || wv->channels == 0 || wv->sample_rate == 0) {
        return 0;
    }

    uint32_t cursor_q16 = ctx->voice_cursor_q16[voice_index];
    uint32_t frame = cursor_q16 >> 16;
    uint32_t frac = cursor_q16 & 0xFFFFU;
    if (frame >= wv->frames) {
        frame %= (uint32_t)wv->frames;
        ctx->voice_cursor_q16[voice_index] = frame << 16;
        cursor_q16 = ctx->voice_cursor_q16[voice_index];
        frac = cursor_q16 & 0xFFFFU;
    }

    uint32_t next_frame = frame + 1;
    if (next_frame >= wv->frames) {
        next_frame = 0;
    }

    int32_t sample_a = 0;
    int32_t sample_b = 0;
    if (wv->channels == 1) {
        sample_a = wv->pcm[frame];
        sample_b = wv->pcm[next_frame];
    } else {
        int32_t la = wv->pcm[frame * 2];
        int32_t ra = wv->pcm[(frame * 2) + 1];
        int32_t lb = wv->pcm[next_frame * 2];
        int32_t rb = wv->pcm[(next_frame * 2) + 1];
        sample_a = (la + ra) / 2;
        sample_b = (lb + rb) / 2;
    }

    int32_t interp = sample_a + (int32_t)(((int64_t)(sample_b - sample_a) * frac) >> 16);

    uint32_t step_q16 = (uint32_t)(((uint64_t)wv->sample_rate << 16) / SAMPLE_RATE);
    if (step_q16 == 0) {
        step_q16 = 1;
    }
    ctx->voice_cursor_q16[voice_index] += step_q16;
    if ((ctx->voice_cursor_q16[voice_index] >> 16) >= wv->frames) {
        ctx->voice_cursor_q16[voice_index] %= (uint32_t)(wv->frames << 16);
    }

    if (interp > 32767) {
        interp = 32767;
    }
    if (interp < -32768) {
        interp = -32768;
    }
    return (int16_t)interp;
}

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

static inline void audio_refresh_enabled(audio_ctx_t *ctx) {
    ctx->enabled = 0;
    for (int i = 0; i < audio_voice_count(); ++i) {
        if (ctx->voice_enabled[i]) {
            ctx->enabled = 1;
            break;
        }
    }
}

static inline int audio_voice_is_enabled(const audio_ctx_t *ctx, int voice_index) {
    if (voice_index < 0 || voice_index >= audio_voice_count()) {
        return 0;
    }

    int enabled = 0;
    pthread_mutex_lock((pthread_mutex_t *)&ctx->state_lock);
    enabled = ctx->voice_enabled[voice_index] ? 1 : 0;
    pthread_mutex_unlock((pthread_mutex_t *)&ctx->state_lock);
    return enabled;
}

static inline void audio_set_voice(audio_ctx_t *ctx, int voice_index, int enabled) {
    if (voice_index < 0 || voice_index >= audio_voice_count()) {
        return;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (enabled && !ctx->voice_enabled[voice_index]) {
        ctx->voice_cursor_q16[voice_index] = 0;
    }
    ctx->voice_enabled[voice_index] = enabled ? 1 : 0;
    audio_refresh_enabled(ctx);
    pthread_mutex_unlock(&ctx->state_lock);
}

static inline int audio_toggle_voice(audio_ctx_t *ctx, int voice_index) {
    if (voice_index < 0 || voice_index >= audio_voice_count()) {
        return 0;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (!ctx->voice_enabled[voice_index]) {
        ctx->voice_cursor_q16[voice_index] = 0;
    }
    ctx->voice_enabled[voice_index] = ctx->voice_enabled[voice_index] ? 0 : 1;
    audio_refresh_enabled(ctx);
    int enabled = ctx->voice_enabled[voice_index] ? 1 : 0;
    pthread_mutex_unlock(&ctx->state_lock);
    return enabled;
}

static inline int audio_open(audio_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->frame_bytes = sizeof(int16_t) * 2;

    if (pthread_mutex_init(&ctx->state_lock, NULL) != 0) {
        printf("[INIT] [ERR] Failed to initialize audio state mutex.\n");
        return -1;
    }

    if (audio_open_pcm_device(ctx) < 0) {
        pthread_mutex_destroy(&ctx->state_lock);
        return -1;
    }

    for (int i = 0; i < audio_voice_count(); ++i) {
        ctx->voice_enabled[i] = 0;
        ctx->voice_gain_q15[i] = 0;
        ctx->voice_cursor_q16[i] = 0;
        if (audio_load_wav_voice(ctx, i, audio_voice_wav_path(i)) < 0) {
            for (int loaded = 0; loaded < i; ++loaded) {
                audio_unload_wav_voice(ctx, loaded);
            }
            close(ctx->fd);
            ctx->fd = -1;
            pthread_mutex_destroy(&ctx->state_lock);
            return -1;
        }
    }

    ctx->voice_enabled[0] = 1;
    ctx->pending_offset = 0;
    ctx->pending_bytes = 0;
    audio_refresh_enabled(ctx);
    printf("[INIT] ALSA playback initialized on %s using WAV voices from /etc/wavs.\n", ctx->pcm_path);
    return 0;
}

static inline void audio_close(audio_ctx_t *ctx) {
    for (int i = 0; i < audio_voice_count(); ++i) {
        audio_unload_wav_voice(ctx, i);
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    pthread_mutex_destroy(&ctx->state_lock);
    ctx->fd = -1;
}

static inline int audio_write_next(audio_ctx_t *ctx) {
    if (ctx->pending_bytes == 0) {
        const uint16_t gain_max = 32767;
        const uint16_t gain_attack = 1024;
        const uint16_t gain_release = 768;

        pthread_mutex_lock(&ctx->state_lock);

        for (int i = 0; i < BUFFER_FRAMES; ++i) {
            int16_t value = 0;

            if (ctx->enabled) {
                int64_t mixed_accum = 0;
                uint32_t active = 0;

                for (int v = 0; v < audio_voice_count(); ++v) {
                    uint16_t g = ctx->voice_gain_q15[v];
                    uint16_t target = ctx->voice_enabled[v] ? gain_max : 0;

                    if (g < target) {
                        uint32_t stepped = (uint32_t)g + gain_attack;
                        g = (stepped > target) ? target : (uint16_t)stepped;
                    } else if (g > target) {
                        g = (g > gain_release) ? (uint16_t)(g - gain_release) : 0;
                        if (g < target) {
                            g = target;
                        }
                    }

                    ctx->voice_gain_q15[v] = g;
                    if (g == 0) {
                        continue;
                    }

                    mixed_accum += (int64_t)audio_voice_next_sample(ctx, v) * (int64_t)g;
                    ++active;
                }

                if (active > 0) {
                    int64_t denom = (int64_t)gain_max * (int64_t)active;
                    int32_t mixed = (int32_t)(mixed_accum / denom);
                    mixed = (mixed * 3) / 4;

                    if (mixed > 32767) {
                        mixed = 32767;
                    }
                    if (mixed < -32768) {
                        mixed = -32768;
                    }
                    value = (int16_t)mixed;
                }
            }

            ctx->block_buffer[i * 2] = value;
            ctx->block_buffer[i * 2 + 1] = value;
        }

        pthread_mutex_unlock(&ctx->state_lock);

        ctx->pending_offset = 0;
        ctx->pending_bytes = audio_block_bytes();
    }

    size_t frame_offset = ctx->pending_offset / ctx->frame_bytes;
    size_t frames_left = ctx->pending_bytes / ctx->frame_bytes;

    struct snd_xferi xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.buf = ((uint8_t *)ctx->block_buffer) + ctx->pending_offset;
    xfer.frames = frames_left;

    int rc = ioctl(ctx->fd, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &xfer);
    snd_pcm_sframes_t frames_written = xfer.result;

    if (rc < 0 && frames_written >= 0) {
        frames_written = -errno;
    }

    (void)frame_offset;

    if (frames_written > 0) {
        size_t bytes_written = (size_t)frames_written * ctx->frame_bytes;
        ctx->pending_offset += bytes_written;
        ctx->pending_bytes -= bytes_written;

        if (ctx->waiting_for_host) {
            ctx->waiting_for_host = 0;
            printf("[INIT] ALSA host stream resumed.\n");
        }

        return 0;
    }

    if (frames_written < 0) {
        int err = (int)(-frames_written);

        if (err == EPIPE || err == ESTRPIPE || err == EIO) {
            (void)ioctl(ctx->fd, SNDRV_PCM_IOCTL_PREPARE);

            if (!ctx->waiting_for_host) {
                ctx->waiting_for_host = 1;
                ctx->wait_notice_ticks = 0;
                printf("[INIT] [WARN] ALSA stream inactive (xrun/suspend). Waiting for host...\n");
            } else if ((++ctx->wait_notice_ticks % 5000) == 0) {
                printf("[INIT] [WARN] Still waiting on ALSA host stream...\n");
            }

            ctx->pending_offset = 0;
            ctx->pending_bytes = 0;
            usleep(1000);
            return 0;
        }

        printf("[INIT] [WARN] ALSA write error: %s (errno %d)\n", strerror(err), err);
        usleep(10000);
        return 0;
    }

    usleep(1000);

    return 0;
}

#endif
