#ifndef AUDIO_VOICE_BANK_H
#define AUDIO_VOICE_BANK_H

#include <dirent.h>

#include "audio/types.h"

#define AUDIO_ROOTFS_WAV_DIR "/audiox/wavs"
#define AUDIO_INITRAM_WAV_DIR "/etc/wavs"

static inline int audio_voice_capacity(void) {
    return AUDIO_MAX_VOICES;
}

static inline int audio_voice_count(const audio_ctx_t *ctx) {
    if (!ctx) {
        return 0;
    }
    return (int)ctx->control.voice_count;
}

static inline uint8_t audio_voice_note_base(const audio_ctx_t *ctx) {
    if (!ctx) {
        return 36;
    }
    return ctx->control.voice_note_base;
}

static inline const char *audio_voice_name(const audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return NULL;
    }
    return ctx->voice_name[voice_index][0] ? ctx->voice_name[voice_index] : NULL;
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

static inline int audio_path_has_wav_suffix(const char *name) {
    if (!name) {
        return 0;
    }

    size_t len = strlen(name);
    if (len < 4) {
        return 0;
    }

    const char *ext = name + (len - 4);
    return (ext[0] == '.' &&
            (ext[1] == 'w' || ext[1] == 'W') &&
            (ext[2] == 'a' || ext[2] == 'A') &&
            (ext[3] == 'v' || ext[3] == 'V'))
               ? 1
               : 0;
}

static inline int audio_path_is_voice_prefix(const char *name) {
    if (!name) {
        return 0;
    }

    return ((name[0] == 'v' || name[0] == 'V') &&
            (name[1] == 'o' || name[1] == 'O') &&
            (name[2] == 'i' || name[2] == 'I') &&
            (name[3] == 'c' || name[3] == 'C') &&
            (name[4] == 'e' || name[4] == 'E'))
               ? 1
               : 0;
}

static inline int audio_strcmp_for_sort(const void *a, const void *b) {
    const char *const *pa = (const char *const *)a;
    const char *const *pb = (const char *const *)b;
    return strcmp(*pa, *pb);
}

static inline int audio_collect_wavs_from_dir(const char *dir,
                                              char paths[AUDIO_MAX_VOICES][256],
                                              char names[AUDIO_MAX_VOICES][AUDIO_MAX_VOICE_NAME],
                                              int require_voice_prefix) {
    DIR *dp = opendir(dir);
    if (!dp) {
        return 0;
    }

    char found[AUDIO_MAX_VOICES][256];
    char found_name[AUDIO_MAX_VOICES][AUDIO_MAX_VOICE_NAME];
    char sort_name[AUDIO_MAX_VOICES][256];
    char *sort_refs[AUDIO_MAX_VOICES];
    int count = 0;

    struct dirent *entry;
    while ((entry = readdir(dp)) != NULL && count < AUDIO_MAX_VOICES) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        if (!audio_path_has_wav_suffix(entry->d_name)) {
            continue;
        }
        if (require_voice_prefix && !audio_path_is_voice_prefix(entry->d_name)) {
            continue;
        }

        int n = snprintf(found[count], sizeof(found[count]), "%s/%s", dir, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(found[count])) {
            continue;
        }

        snprintf(found_name[count], sizeof(found_name[count]), "%.47s", entry->d_name);
        snprintf(sort_name[count], sizeof(sort_name[count]), "%s", entry->d_name);
        sort_refs[count] = sort_name[count];
        ++count;
    }

    closedir(dp);

    if (count == 0) {
        return 0;
    }

    qsort(sort_refs, (size_t)count, sizeof(sort_refs[0]), audio_strcmp_for_sort);

    int out = 0;
    for (int i = 0; i < count && out < AUDIO_MAX_VOICES; ++i) {
        for (int j = 0; j < count; ++j) {
            if (strcmp(sort_refs[i], sort_name[j]) == 0) {
                snprintf(paths[out], 256, "%s", found[j]);
                snprintf(names[out], AUDIO_MAX_VOICE_NAME, "%s", found_name[j]);
                ++out;
                break;
            }
        }
    }

    return out;
}

static inline void audio_unload_wav_voice(audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_capacity()) {
        return;
    }

    free(ctx->wav[voice_index].pcm);
    ctx->wav[voice_index].pcm = NULL;
    ctx->wav[voice_index].frames = 0;
    ctx->wav[voice_index].sample_rate = 0;
    ctx->wav[voice_index].channels = 0;
    ctx->control.voice_cursor_q16[voice_index] = 0;
    ctx->control.voice_enabled[voice_index] = 0;
    ctx->control.voice_gain_q15[voice_index] = 0;
    ctx->control.voice_loop[voice_index] = 0;
    ctx->voice_name[voice_index][0] = '\0';
}

static inline int audio_load_wav_voice(audio_ctx_t *ctx,
                                       int voice_index,
                                       const char *path,
                                       const char *name) {
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
    ctx->control.voice_cursor_q16[voice_index] = 0;
    ctx->control.voice_enabled[voice_index] = 0;
    ctx->control.voice_gain_q15[voice_index] = 0;
    ctx->control.voice_loop[voice_index] = 0;
    snprintf(ctx->voice_name[voice_index], sizeof(ctx->voice_name[voice_index]), "%s", name ? name : path);

    printf("[INIT] Loaded voice %d WAV: %s (%u Hz, %u ch, %zu frames)\n",
           voice_index,
           path,
           (unsigned)sample_rate,
           (unsigned)channels,
           frames);
    return 0;
}

static inline int audio_load_voice_bank(audio_ctx_t *ctx) {
    if (!ctx) {
        return -1;
    }

    char paths[AUDIO_MAX_VOICES][256];
    char names[AUDIO_MAX_VOICES][AUDIO_MAX_VOICE_NAME];
    int loaded = audio_collect_wavs_from_dir(AUDIO_ROOTFS_WAV_DIR, paths, names, 0);
    const char *source_dir = AUDIO_ROOTFS_WAV_DIR;

    if (loaded == 0) {
        loaded = audio_collect_wavs_from_dir(AUDIO_INITRAM_WAV_DIR, paths, names, 1);
        source_dir = AUDIO_INITRAM_WAV_DIR;
    }

    if (loaded == 0) {
        printf("[INIT] [ERR] No WAV voices found in %s or %s\n", AUDIO_ROOTFS_WAV_DIR, AUDIO_INITRAM_WAV_DIR);
        return -1;
    }

    for (int i = 0; i < audio_voice_capacity(); ++i) {
        audio_unload_wav_voice(ctx, i);
    }

    int ok = 0;
    for (int i = 0; i < loaded; ++i) {
        if (audio_load_wav_voice(ctx, i, paths[i], names[i]) == 0) {
            ++ok;
        }
    }

    if (ok == 0) {
        printf("[INIT] [ERR] Could not load any WAV voices from %s\n", source_dir);
        return -1;
    }

    ctx->control.voice_count = (uint8_t)ok;
    printf("[INIT] Loaded %d voice(s) from %s\n", ok, source_dir);
    return 0;
}

static inline int16_t audio_voice_next_sample(audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return 0;
    }

    struct wav_voice *wv = &ctx->wav[voice_index];
    if (!wv->pcm || wv->frames == 0 || wv->channels == 0 || wv->sample_rate == 0) {
        return 0;
    }

    uint32_t cursor_q16 = ctx->control.voice_cursor_q16[voice_index];
    uint32_t frame = cursor_q16 >> 16;
    uint32_t frac = cursor_q16 & 0xFFFFU;

    if (frame >= wv->frames) {
        if (ctx->control.voice_loop[voice_index]) {
            frame %= (uint32_t)wv->frames;
            ctx->control.voice_cursor_q16[voice_index] = frame << 16;
            cursor_q16 = ctx->control.voice_cursor_q16[voice_index];
            frac = cursor_q16 & 0xFFFFU;
        } else {
            ctx->control.voice_enabled[voice_index] = 0;
            ctx->control.voice_gain_q15[voice_index] = 0;
            return 0;
        }
    }

    uint32_t next_frame = frame + 1;
    if (next_frame >= wv->frames) {
        next_frame = ctx->control.voice_loop[voice_index] ? 0 : frame;
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
    ctx->control.voice_cursor_q16[voice_index] += step_q16;

    if ((ctx->control.voice_cursor_q16[voice_index] >> 16) >= wv->frames && !ctx->control.voice_loop[voice_index]) {
        ctx->control.voice_enabled[voice_index] = 0;
    } else if ((ctx->control.voice_cursor_q16[voice_index] >> 16) >= wv->frames && ctx->control.voice_loop[voice_index]) {
        ctx->control.voice_cursor_q16[voice_index] %= (uint32_t)(wv->frames << 16);
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
