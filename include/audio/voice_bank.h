#ifndef AUDIO_VOICE_BANK_H
#define AUDIO_VOICE_BANK_H

#include "audio/types.h"

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

#endif
