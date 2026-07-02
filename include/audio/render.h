#ifndef AUDIO_RENDER_H
#define AUDIO_RENDER_H

#include "audio/alsa_pcm.h"
#include "audio/control.h"

static inline size_t audio_block_bytes(void) {
    return sizeof(int16_t) * BUFFER_FRAMES * 2;
}

static inline int audio_open(audio_ctx_t *ctx) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;
    ctx->input_fd = -1;
    ctx->frame_bytes = sizeof(int16_t) * 2;

    if (pthread_mutex_init(&ctx->state_lock, NULL) != 0) {
        printf("[INIT] [ERR] Failed to initialize audio state mutex.\n");
        return -1;
    }

    if (audio_open_pcm_device(ctx) < 0) {
        pthread_mutex_destroy(&ctx->state_lock);
        return -1;
    }

    ctx->control.voice_note_base = 36;

    if (audio_load_voice_bank(ctx) < 0) {
        close(ctx->fd);
        ctx->fd = -1;
        pthread_mutex_destroy(&ctx->state_lock);
        return -1;
    }

    ctx->pending_offset = 0;
    ctx->pending_bytes = 0;
    audio_refresh_enabled(ctx);
    if (audio_try_open_input_device(ctx) < 0) {
        printf("[INIT] [WARN] USB mic input not available at startup. Continuing without live input.\n");
    }
    printf("[INIT] ALSA playback initialized on %s using dynamic voice bank.\n", ctx->pcm_path);
    return 0;
}

static inline void audio_close(audio_ctx_t *ctx) {
    for (int i = 0; i < audio_voice_capacity(); ++i) {
        audio_unload_wav_voice(ctx, i);
    }
    audio_close_input_device(ctx, NULL);
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
        audio_control_state_t *control = &ctx->control;
        int16_t input_mix[BUFFER_FRAMES];

        (void)audio_capture_read_mix(ctx, input_mix, BUFFER_FRAMES);

        pthread_mutex_lock(&ctx->state_lock);

        for (int i = 0; i < BUFFER_FRAMES; ++i) {
            int32_t value = input_mix[i];

            if (control->enabled) {
                int64_t mixed_accum = 0;
                uint32_t active = 0;

                for (int v = 0; v < audio_voice_count(ctx); ++v) {
                    uint16_t g = control->voice_gain_q15[v];
                    uint16_t target = control->voice_enabled[v] ? gain_max : 0;

                    if (g < target) {
                        uint32_t stepped = (uint32_t)g + gain_attack;
                        g = (stepped > target) ? target : (uint16_t)stepped;
                    } else if (g > target) {
                        g = (g > gain_release) ? (uint16_t)(g - gain_release) : 0;
                        if (g < target) {
                            g = target;
                        }
                    }

                    control->voice_gain_q15[v] = g;
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
                    value += mixed;
                }
            }

            if (value > 32767) {
                value = 32767;
            }
            if (value < -32768) {
                value = -32768;
            }

            ctx->block_buffer[i * 2] = (int16_t)value;
            ctx->block_buffer[i * 2 + 1] = (int16_t)value;
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
