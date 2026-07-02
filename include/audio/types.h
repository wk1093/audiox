#ifndef AUDIO_TYPES_H
#define AUDIO_TYPES_H

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

#define AUDIO_MAX_VOICES 16
#define AUDIO_MAX_VOICE_NAME 48

typedef struct audio_ctx {
    int fd;
    int enabled;
    pthread_mutex_t state_lock;
    char pcm_path[64];
    snd_pcm_uframes_t period_frames;
    size_t frame_bytes;
    uint8_t voice_enabled[AUDIO_MAX_VOICES];
    uint8_t voice_loop[AUDIO_MAX_VOICES];
    uint16_t voice_gain_q15[AUDIO_MAX_VOICES];
    struct wav_voice {
        int16_t *pcm;
        size_t frames;
        uint32_t sample_rate;
        uint16_t channels;
    } wav[AUDIO_MAX_VOICES];
    char voice_name[AUDIO_MAX_VOICES][AUDIO_MAX_VOICE_NAME];
    uint32_t voice_cursor_q16[AUDIO_MAX_VOICES];
    uint8_t voice_count;
    uint8_t voice_note_base;
    int16_t block_buffer[BUFFER_FRAMES * 2];
    size_t pending_offset;
    size_t pending_bytes;
    int warm_up_ticks;
    int waiting_for_host;
    uint32_t wait_notice_ticks;
} audio_ctx_t;

#endif
