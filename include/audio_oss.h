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
    uint32_t sample_index;
    int warm_up_ticks;
} audio_ctx_t;

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

    ctx->enabled = 1;
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
            double sample_value = AMPLITUDE * sin(2.0 * PI * FREQUENCY * (double)ctx->sample_index / SAMPLE_RATE);
            value = (int16_t)sample_value;
            ctx->sample_index++;
            if (ctx->sample_index >= SAMPLE_RATE) {
                ctx->sample_index = 0;
            }
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
