#ifndef AUDIO_OSS_H
#define AUDIO_OSS_H

#include <errno.h>
#include <fcntl.h>
#include <linux/soundcard.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "init_defs.h"

typedef struct audio_ctx {
    int fd;
    int enabled;
    uint8_t voice_enabled[4];
    double voice_phase[4];
    int warm_up_ticks;
} audio_ctx_t;

static inline int audio_voice_count(void) {
    return 4;
}

static inline double audio_voice_frequency(int voice_index) {
    static const double freqs[4] = {220.0, 330.0, 440.0, 660.0};
    if (voice_index < 0 || voice_index >= (int)(sizeof(freqs) / sizeof(freqs[0]))) {
        return 440.0;
    }
    return freqs[voice_index];
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
    return ctx->voice_enabled[voice_index] ? 1 : 0;
}

static inline void audio_set_voice(audio_ctx_t *ctx, int voice_index, int enabled) {
    if (voice_index < 0 || voice_index >= audio_voice_count()) {
        return;
    }
    ctx->voice_enabled[voice_index] = enabled ? 1 : 0;
    audio_refresh_enabled(ctx);
}

static inline int audio_toggle_voice(audio_ctx_t *ctx, int voice_index) {
    if (voice_index < 0 || voice_index >= audio_voice_count()) {
        return 0;
    }
    ctx->voice_enabled[voice_index] = ctx->voice_enabled[voice_index] ? 0 : 1;
    audio_refresh_enabled(ctx);
    return ctx->voice_enabled[voice_index] ? 1 : 0;
}

static inline int audio_open(audio_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = open("/dev/dsp", O_WRONLY);
    if (ctx->fd < 0) {
        perror("[CRITICAL] Could not open /dev/dsp. Is snd-pcm-oss loaded?");
        return -1;
    }

    int format = AFMT_S16_LE;
    int channels = 2;
    int speed = SAMPLE_RATE;

    if (ioctl(ctx->fd, SNDCTL_DSP_SETFMT, &format) < 0) {
        perror("[WARN] Failed to set OSS audio format");
    }
    if (ioctl(ctx->fd, SNDCTL_DSP_CHANNELS, &channels) < 0) {
        perror("[WARN] Failed to set OSS channel count");
    }
    if (ioctl(ctx->fd, SNDCTL_DSP_SPEED, &speed) < 0) {
        perror("[WARN] Failed to set OSS hardware speed");
    }

    for (int i = 0; i < audio_voice_count(); ++i) {
        ctx->voice_enabled[i] = (i == 0) ? 1 : 0;
        ctx->voice_phase[i] = 0.0;
    }
    audio_refresh_enabled(ctx);
    printf("[INIT] /dev/dsp initialized to 16-bit Stereo @ %dHz.\n", SAMPLE_RATE);
    return 0;
}

static inline void audio_close(audio_ctx_t *ctx) {
    if (ctx->fd >= 0) {
        close(ctx->fd);
    }
    ctx->fd = -1;
}

static inline int audio_write_next(audio_ctx_t *ctx) {
    int16_t block_buffer[BUFFER_FRAMES * 2];

    for (int i = 0; i < BUFFER_FRAMES; ++i) {
        int16_t value = 0;

        if (ctx->enabled) {
            double mixed = 0.0;

            for (int v = 0; v < audio_voice_count(); ++v) {
                if (!ctx->voice_enabled[v]) {
                    continue;
                }

                mixed += sin(ctx->voice_phase[v]);
                ctx->voice_phase[v] += (2.0 * PI * audio_voice_frequency(v)) / SAMPLE_RATE;
                if (ctx->voice_phase[v] >= 2.0 * PI) {
                    ctx->voice_phase[v] -= 2.0 * PI;
                }
            }

            double sample_value = (AMPLITUDE * 0.35) * mixed;
            if (sample_value > 32767.0) sample_value = 32767.0;
            if (sample_value < -32768.0) sample_value = -32768.0;
            value = (int16_t)sample_value;
        }

        block_buffer[i * 2] = value;
        block_buffer[i * 2 + 1] = value;
    }

    ssize_t bytes_to_write = (ssize_t)sizeof(block_buffer);
    ssize_t bytes_written = write(ctx->fd, block_buffer, (size_t)bytes_to_write);

    if (bytes_written < 0) {
        if (errno == EIO) {
            if ((ctx->warm_up_ticks++ % 100) == 0) {
                printf("[INIT] [WARN] /dev/dsp busy or host stream inactive. Waiting for host...\n");
            }
            usleep(10000);
            return 0;
        }

        printf("[INIT] [WARN] Write error: %s (errno %d)\n", strerror(errno), errno);
        usleep(10000);
        return 0;
    }

    return 0;
}

#endif
