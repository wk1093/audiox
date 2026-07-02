#ifndef AUDIO_ALSA_PCM_H
#define AUDIO_ALSA_PCM_H

#include <dirent.h>

#include "audio/types.h"

#define AUDIO_MAX_PCM_CANDIDATES 32
#define AUDIO_INPUT_PROBE_BLOCKS 32
#define AUDIO_INPUT_FALLBACK_RATE 48000U
#define AUDIO_CAPTURE_PERIOD_FRAMES 256U
#define AUDIO_CAPTURE_PERIODS 4U
#define AUDIO_INPUT_MAX_CAPTURE_FRAMES ((((BUFFER_FRAMES) * (AUDIO_INPUT_FALLBACK_RATE)) / (SAMPLE_RATE)) + 16U)

typedef struct audio_pcm_candidate {
    char path[PATH_MAX];
    int card_index;
    int is_usb;
} audio_pcm_candidate_t;

static inline void audio_copy_pcm_path(char *dst, size_t dst_size, const char *src) {
    if (!dst || dst_size == 0) {
        return;
    }

    if (!src) {
        dst[0] = '\0';
        return;
    }

    size_t len = strnlen(src, dst_size - 1);
    memcpy(dst, src, len);
    dst[len] = '\0';
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

static inline int audio_pcm_name_matches_direction(const char *name, char dir_char) {
    if (!name || strncmp(name, "pcmC", 4) != 0) {
        return 0;
    }

    const char *d = strchr(name + 4, 'D');
    if (!d || d == name + 4 || d[1] == '\0') {
        return 0;
    }

    return name[strlen(name) - 1] == dir_char ? 1 : 0;
}

static inline int audio_pcm_card_index_from_name(const char *name) {
    if (!audio_pcm_name_matches_direction(name, name ? name[strlen(name) - 1] : '\0')) {
        return -1;
    }

    const char *start = name + 4;
    const char *end = strchr(start, 'D');
    if (!end) {
        return -1;
    }

    int value = 0;
    for (const char *p = start; p < end; ++p) {
        if (*p < '0' || *p > '9') {
            return -1;
        }
        value = (value * 10) + (*p - '0');
    }
    return value;
}

static inline int audio_pcm_card_index_from_path(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    const char *name = strrchr(path, '/');
    return audio_pcm_card_index_from_name(name ? (name + 1) : path);
}

static inline int audio_pcm_device_is_usb(const char *name) {
    if (!name || !name[0]) {
        return 0;
    }

    char sysfs_path[PATH_MAX];
    char resolved[PATH_MAX];
    int n = snprintf(sysfs_path, sizeof(sysfs_path), "/sys/class/sound/%s/device", name);
    if (n <= 0 || (size_t)n >= sizeof(sysfs_path)) {
        return 0;
    }

    if (!realpath(sysfs_path, resolved)) {
        return 0;
    }

    return strstr(resolved, "/usb") ? 1 : 0;
}

static inline int audio_pcm_candidate_cmp(const audio_pcm_candidate_t *a,
                                          const audio_pcm_candidate_t *b,
                                          int prefer_usb) {
    int a_rank = prefer_usb ? (a->is_usb ? 0 : 1) : (a->is_usb ? 1 : 0);
    int b_rank = prefer_usb ? (b->is_usb ? 0 : 1) : (b->is_usb ? 1 : 0);

    if (a_rank != b_rank) {
        return a_rank - b_rank;
    }
    if (a->card_index != b->card_index) {
        return a->card_index - b->card_index;
    }
    return strcmp(a->path, b->path);
}

static inline size_t audio_collect_pcm_candidates(char dir_char,
                                                  int prefer_usb,
                                                  int avoid_card,
                                                  audio_pcm_candidate_t *out,
                                                  size_t out_cap) {
    DIR *dir = opendir("/dev/snd");
    if (!dir) {
        return 0;
    }

    size_t count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < out_cap) {
        if (!audio_pcm_name_matches_direction(entry->d_name, dir_char)) {
            continue;
        }

        int card_index = audio_pcm_card_index_from_name(entry->d_name);
        if (card_index < 0 || (avoid_card >= 0 && card_index == avoid_card)) {
            continue;
        }

        int n = snprintf(out[count].path, sizeof(out[count].path), "/dev/snd/%s", entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(out[count].path)) {
            continue;
        }

        out[count].card_index = card_index;
        out[count].is_usb = audio_pcm_device_is_usb(entry->d_name);
        ++count;
    }

    closedir(dir);

    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (audio_pcm_candidate_cmp(&out[j], &out[i], prefer_usb) < 0) {
                audio_pcm_candidate_t tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    }

    return count;
}

static inline int audio_pcm_configure_hw_fd(int fd,
                                            const char *path,
                                            unsigned int rate,
                                            unsigned int channels,
                                            unsigned int period_frames,
                                            unsigned int periods,
                                            snd_pcm_uframes_t *period_frames_out,
                                            size_t *frame_bytes_out,
                                            int configure_timing,
                                            int log_fail) {
    struct snd_pcm_hw_params hw;
    audio_pcm_hw_params_any(&hw);

    const unsigned int buffer_frames = period_frames * periods;

    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_FORMAT, SNDRV_PCM_FORMAT_S16_LE);
    audio_pcm_hw_params_set_mask(&hw, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);

    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_CHANNELS, channels, channels, 1);
    audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_RATE, rate, rate, 1);
    if (configure_timing) {
        audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period_frames, period_frames, 1);
        audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_PERIODS, periods, periods, 1);
        audio_pcm_hw_params_set_interval(&hw, SNDRV_PCM_HW_PARAM_BUFFER_SIZE, buffer_frames, buffer_frames, 1);
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_HW_PARAMS, &hw) < 0) {
        if (log_fail) {
            printf("[INIT] [ERR] ALSA HW_PARAMS failed on %s: %s\n", path, strerror(errno));
        }
        return -1;
    }

    if (ioctl(fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
        if (log_fail) {
            printf("[INIT] [ERR] ALSA PREPARE failed on %s: %s\n", path, strerror(errno));
        }
        return -1;
    }

    if (period_frames_out) {
        *period_frames_out = period_frames;
    }
    if (frame_bytes_out) {
        *frame_bytes_out = sizeof(int16_t) * channels;
    }
    return 0;
}

static inline int16_t audio_capture_frame_mono(const int16_t *raw, size_t frame_index, uint8_t channels) {
    if (!raw || channels == 0) {
        return 0;
    }

    int32_t sample = raw[frame_index * channels];
    if (channels > 1) {
        sample += raw[(frame_index * channels) + 1];
        sample /= 2;
    }

    if (sample > 32767) {
        sample = 32767;
    }
    if (sample < -32768) {
        sample = -32768;
    }
    return (int16_t)sample;
}

static inline int audio_try_open_playback_candidate(audio_ctx_t *ctx, const char *path) {
    static const struct {
        unsigned int period_frames;
        unsigned int periods;
    } playback_profiles[] = {
        {BUFFER_FRAMES, 2},
        {256, 2},
        {256, 4},
        {512, 4}
    };

    int fd = open(path, O_WRONLY);
    if (fd < 0) {
        return -1;
    }

    for (size_t i = 0; i < (sizeof(playback_profiles) / sizeof(playback_profiles[0])); ++i) {
        if (audio_pcm_configure_hw_fd(fd,
                                      path,
                                      SAMPLE_RATE,
                                      2,
                                      playback_profiles[i].period_frames,
                                      playback_profiles[i].periods,
                                      &ctx->period_frames,
                                      &ctx->frame_bytes,
                                      1,
                                      0) == 0) {
            ctx->fd = fd;
            audio_copy_pcm_path(ctx->pcm_path, sizeof(ctx->pcm_path), path);
            printf("[INIT] ALSA PCM opened: %s (S16_LE, 2ch, %d Hz, period=%u periods=%u)\n",
                   ctx->pcm_path,
                   SAMPLE_RATE,
                   (unsigned)ctx->period_frames,
                   playback_profiles[i].periods);
            return 0;
        }
    }

    printf("[INIT] [ERR] ALSA playback rejected on %s for all timing profiles.\n", path);

    close(fd);
    return -1;
}

static inline int audio_open_pcm_device(audio_ctx_t *ctx) {
    audio_pcm_candidate_t candidates[AUDIO_MAX_PCM_CANDIDATES];
    size_t count = audio_collect_pcm_candidates('p', 0, -1, candidates, AUDIO_MAX_PCM_CANDIDATES);

    for (size_t i = 0; i < count; ++i) {
        if (audio_try_open_playback_candidate(ctx, candidates[i].path) == 0) {
            return 0;
        }
    }

    printf("[INIT] [ERR] No usable ALSA playback PCM found under /dev/snd\n");
    return -1;
}

static inline void audio_close_input_device(audio_ctx_t *ctx, const char *reason) {
    if (!ctx) {
        return;
    }

    if (ctx->input_fd >= 0) {
        close(ctx->input_fd);
        ctx->input_fd = -1;
    }

    if (ctx->input_pcm_path[0] && reason && reason[0]) {
        printf("[INIT] %s: %s\n", reason, ctx->input_pcm_path);
    }

    ctx->input_pcm_path[0] = '\0';
    ctx->input_channels = 0;
    ctx->input_sample_rate = 0;
    ctx->input_resample_pos_q16 = 0;
    ctx->input_last_mono_sample = 0;
    ctx->input_have_last_sample = 0;
    ctx->input_probe_ticks = AUDIO_INPUT_PROBE_BLOCKS;
}

static inline int audio_try_open_input_candidate(audio_ctx_t *ctx, const char *path) {
    const unsigned int channel_attempts[] = {1, 2};
    const unsigned int rate_attempts[] = {SAMPLE_RATE, AUDIO_INPUT_FALLBACK_RATE};

    for (size_t rate_idx = 0; rate_idx < (sizeof(rate_attempts) / sizeof(rate_attempts[0])); ++rate_idx) {
        for (size_t ch_idx = 0; ch_idx < (sizeof(channel_attempts) / sizeof(channel_attempts[0])); ++ch_idx) {
            int fd = open(path, O_RDONLY | O_NONBLOCK);
            if (fd < 0) {
                return -1;
            }

            snd_pcm_uframes_t input_period = 0;
            size_t input_frame_bytes = 0;
            if (audio_pcm_configure_hw_fd(fd,
                                          path,
                                          rate_attempts[rate_idx],
                                          channel_attempts[ch_idx],
                                          AUDIO_CAPTURE_PERIOD_FRAMES,
                                          AUDIO_CAPTURE_PERIODS,
                                          &input_period,
                                          &input_frame_bytes,
                                          1,
                                          0) == 0) {
                ctx->input_fd = fd;
                ctx->input_channels = (uint8_t)channel_attempts[ch_idx];
                ctx->input_sample_rate = rate_attempts[rate_idx];
                ctx->input_resample_pos_q16 = 0;
                ctx->input_last_mono_sample = 0;
                ctx->input_have_last_sample = 0;
                audio_copy_pcm_path(ctx->input_pcm_path, sizeof(ctx->input_pcm_path), path);
                ctx->input_probe_ticks = 0;
                  printf("[INIT] Audio input connected: %s (%uch, %u Hz, period=%u periods=%u)\n",
                       ctx->input_pcm_path,
                       (unsigned)ctx->input_channels,
                      (unsigned)ctx->input_sample_rate,
                      (unsigned)AUDIO_CAPTURE_PERIOD_FRAMES,
                      (unsigned)AUDIO_CAPTURE_PERIODS);
                return 0;
            }

            close(fd);
        }
    }

    printf("[INIT] [WARN] Audio input candidate rejected: %s (need S16_LE mono/stereo at 44100 or 48000 Hz)\n",
           path);

    return -1;
}

static inline int audio_try_open_input_device(audio_ctx_t *ctx) {
    if (!ctx || ctx->input_fd >= 0) {
        return 0;
    }

    audio_pcm_candidate_t candidates[AUDIO_MAX_PCM_CANDIDATES];
    int playback_card = audio_pcm_card_index_from_path(ctx->pcm_path);
    size_t count = audio_collect_pcm_candidates('c', 1, playback_card, candidates, AUDIO_MAX_PCM_CANDIDATES);

    for (size_t i = 0; i < count; ++i) {
        if (!candidates[i].is_usb) {
            continue;
        }
        if (audio_try_open_input_candidate(ctx, candidates[i].path) == 0) {
            return 0;
        }
    }

    return -1;
}

static inline void audio_probe_input_device(audio_ctx_t *ctx) {
    if (!ctx || ctx->input_fd >= 0) {
        return;
    }

    if (++ctx->input_probe_ticks < AUDIO_INPUT_PROBE_BLOCKS) {
        return;
    }

    ctx->input_probe_ticks = 0;
    (void)audio_try_open_input_device(ctx);
}

static inline int audio_capture_read_mix(audio_ctx_t *ctx, int16_t *mix_out, size_t frames) {
    if (!mix_out) {
        return 0;
    }

    memset(mix_out, 0, sizeof(*mix_out) * frames);

    if (!ctx) {
        return 0;
    }

    if (ctx->input_fd < 0) {
        audio_probe_input_device(ctx);
        return 0;
    }

    size_t capture_frames = frames;
    if (ctx->input_sample_rate > 0 && ctx->input_sample_rate != SAMPLE_RATE) {
        capture_frames = (size_t)((((uint64_t)frames * (uint64_t)ctx->input_sample_rate) + SAMPLE_RATE - 1U) / SAMPLE_RATE) + 2U;
    }
    if (capture_frames > AUDIO_INPUT_MAX_CAPTURE_FRAMES) {
        capture_frames = AUDIO_INPUT_MAX_CAPTURE_FRAMES;
    }

    int16_t raw[AUDIO_INPUT_MAX_CAPTURE_FRAMES * 2];
    memset(raw, 0, sizeof(raw));

    struct snd_xferi xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.buf = raw;
    xfer.frames = capture_frames;

    int rc = ioctl(ctx->input_fd, SNDRV_PCM_IOCTL_READI_FRAMES, &xfer);
    snd_pcm_sframes_t frames_read = xfer.result;

    if (rc < 0 && frames_read >= 0) {
        frames_read = -errno;
    }

    if (frames_read > 0) {
        uint32_t input_rate = ctx->input_sample_rate ? ctx->input_sample_rate : SAMPLE_RATE;
        size_t out_filled = 0;

        if (input_rate == SAMPLE_RATE) {
            size_t limit = (size_t)frames_read < frames ? (size_t)frames_read : frames;
            for (size_t i = 0; i < limit; ++i) {
                mix_out[i] = audio_capture_frame_mono(raw, i, ctx->input_channels);
                ctx->input_last_mono_sample = mix_out[i];
                ctx->input_have_last_sample = 1;
            }
            out_filled = limit;
        } else if (frames_read > 1) {
            uint32_t step_q16 = (uint32_t)(((uint64_t)input_rate << 16) / SAMPLE_RATE);
            uint32_t pos_q16 = ctx->input_resample_pos_q16;

            for (size_t out_idx = 0; out_idx < frames; ++out_idx) {
                uint32_t src_idx = pos_q16 >> 16;
                if ((size_t)(src_idx + 1U) >= (size_t)frames_read) {
                    break;
                }

                uint32_t frac = pos_q16 & 0xFFFFU;
                int32_t s0 = audio_capture_frame_mono(raw, src_idx, ctx->input_channels);
                int32_t s1 = audio_capture_frame_mono(raw, src_idx + 1U, ctx->input_channels);
                int32_t interp = s0 + (int32_t)(((int64_t)(s1 - s0) * frac) >> 16);

                if (interp > 32767) {
                    interp = 32767;
                }
                if (interp < -32768) {
                    interp = -32768;
                }

                mix_out[out_idx] = (int16_t)interp;
                ctx->input_last_mono_sample = mix_out[out_idx];
                ctx->input_have_last_sample = 1;
                out_filled = out_idx + 1U;
                pos_q16 += step_q16;
            }

            ctx->input_resample_pos_q16 = pos_q16 & 0xFFFFU;
        } else {
            mix_out[0] = audio_capture_frame_mono(raw, 0, ctx->input_channels);
            ctx->input_last_mono_sample = mix_out[0];
            ctx->input_have_last_sample = 1;
            out_filled = 1;
        }

        if (out_filled < frames && ctx->input_have_last_sample) {
            for (size_t i = out_filled; i < frames; ++i) {
                mix_out[i] = ctx->input_last_mono_sample;
            }
        }

        return (int)frames_read;
    }

    if (frames_read == 0 || frames_read == -ENODEV || frames_read == -EBADFD) {
        audio_close_input_device(ctx, "[INIT] Audio input disconnected");
        return 0;
    }

    if (frames_read == -EAGAIN || frames_read == -EWOULDBLOCK) {
        return 0;
    }

    if (frames_read == -EPIPE || frames_read == -ESTRPIPE || frames_read == -EIO) {
        if (ioctl(ctx->input_fd, SNDRV_PCM_IOCTL_PREPARE) < 0) {
            audio_close_input_device(ctx, "[INIT] Audio input dropped after capture xrun");
        }
        return 0;
    }

    printf("[INIT] [WARN] Audio input read error on %s: %s\n",
           ctx->input_pcm_path,
           strerror((int)(-frames_read)));
    audio_close_input_device(ctx, "[INIT] Audio input closed after read error");
    return 0;
}

#endif
