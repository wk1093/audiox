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
    int16_t block_buffer[128 * 2];
    size_t pending_offset;
    size_t pending_bytes;
    int warm_up_ticks;
    int waiting_for_host;
    uint32_t wait_notice_ticks;
} audio_ctx_t;

static inline size_t audio_block_bytes(void) {
    return sizeof(int16_t) * 128 * 2;
}

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
    ctx->fd = open("/dev/dsp", O_WRONLY | O_NONBLOCK);
    if (ctx->fd < 0) {
        perror("[CRITICAL] Could not open /dev/dsp. Is snd-pcm-oss loaded?");
        return -1;
    }

    int format = AFMT_S16_LE;
    int channels = 2;
    int speed = SAMPLE_RATE;
    int frag = (4 << 16) | 9; // 4 fragments of 512 bytes each (lower latency)

    (void)ioctl(ctx->fd, SNDCTL_DSP_SETFRAGMENT, &frag);

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
    ctx->pending_offset = 0;
    ctx->pending_bytes = 0;
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
    if (ctx->pending_bytes == 0) {
        for (int i = 0; i < 128; ++i) {
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

            ctx->block_buffer[i * 2] = value;
            ctx->block_buffer[i * 2 + 1] = value;
        }

        ctx->pending_offset = 0;
        ctx->pending_bytes = audio_block_bytes();
    }

    uint8_t *write_ptr = ((uint8_t *)ctx->block_buffer) + ctx->pending_offset;
    ssize_t bytes_written = write(ctx->fd, write_ptr, ctx->pending_bytes);

    if (bytes_written > 0) {
        ctx->pending_offset += (size_t)bytes_written;
        ctx->pending_bytes -= (size_t)bytes_written;

        if (ctx->waiting_for_host) {
            ctx->waiting_for_host = 0;
            printf("[INIT] /dev/dsp host stream resumed.\n");
        }

        return 0;
    }

    if (bytes_written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // Normal non-blocking backpressure from OSS; do not treat as host-down.
            usleep(200);
            return 0;
        }

        if (errno == EIO) {
            if (!ctx->waiting_for_host) {
                ctx->waiting_for_host = 1;
                ctx->wait_notice_ticks = 0;
                printf("[INIT] [WARN] /dev/dsp busy or host stream inactive. Waiting for host...\n");
            } else if ((++ctx->wait_notice_ticks % 5000) == 0) {
                printf("[INIT] [WARN] Still waiting on /dev/dsp host stream...\n");
            }

            // Drop any queued block so old audio is not emitted when host resumes.
            ctx->pending_offset = 0;
            ctx->pending_bytes = 0;
            usleep(1000);
            return 0;
        }

        printf("[INIT] [WARN] Write error: %s (errno %d)\n", strerror(errno), errno);
        usleep(10000);
        return 0;
    }

    return 0;
}

#endif
