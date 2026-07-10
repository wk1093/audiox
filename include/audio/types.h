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

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#define AUDIO_MAX_VOICES 16
#define AUDIO_MAX_VOICE_NAME 48
#define AUDIO_INPUT_RING_FRAMES (BUFFER_FRAMES * 64)

typedef enum audio_device_role {
    AUDIO_DEVICE_ROLE_PLAYBACK = 0,
    AUDIO_DEVICE_ROLE_CAPTURE = 1
} audio_device_role_t;

typedef enum audio_device_kind {
    AUDIO_DEVICE_KIND_UNKNOWN = 0,
    AUDIO_DEVICE_KIND_USB_OTG_GADGET = 1,
    AUDIO_DEVICE_KIND_USB_MIC = 2,
    AUDIO_DEVICE_KIND_PCM3168 = 3
} audio_device_kind_t;

typedef enum audio_device_transport {
    AUDIO_DEVICE_TRANSPORT_UNKNOWN = 0,
    AUDIO_DEVICE_TRANSPORT_USB_OTG = 1,
    AUDIO_DEVICE_TRANSPORT_USB = 2,
    AUDIO_DEVICE_TRANSPORT_I2S = 3,
    AUDIO_DEVICE_TRANSPORT_SOC = 4
} audio_device_transport_t;

typedef enum audio_device_backend {
    AUDIO_DEVICE_BACKEND_NONE = 0,
    AUDIO_DEVICE_BACKEND_ALSA = 1
} audio_device_backend_t;

typedef struct audio_endpoint {
    audio_device_role_t role;
    audio_device_kind_t kind;
    audio_device_transport_t transport;
    audio_device_backend_t backend;
    char path[PATH_MAX];
    int card_index;
    uint8_t is_usb;
    uint8_t channels;
    uint8_t sample_size_bytes;
    uint32_t sample_rate;
} audio_endpoint_t;

typedef struct audio_control_state {
    int enabled;
    uint8_t voice_enabled[AUDIO_MAX_VOICES];
    uint8_t voice_loop[AUDIO_MAX_VOICES];
    uint16_t voice_gain_q15[AUDIO_MAX_VOICES];
    uint32_t voice_cursor_q16[AUDIO_MAX_VOICES];
    uint32_t voice_revision[AUDIO_MAX_VOICES];
    uint8_t voice_count;
    uint8_t voice_note_base;
} audio_control_state_t;

typedef struct audio_control_snapshot {
    int enabled;
    uint8_t voice_enabled[AUDIO_MAX_VOICES];
    uint8_t voice_count;
    uint8_t voice_note_base;
} audio_control_snapshot_t;

typedef struct audio_ctx {
    int fd;
    int input_fd;
    pthread_mutex_t state_lock;
    char pcm_path[PATH_MAX];
    char input_pcm_path[PATH_MAX];
    audio_endpoint_t playback_endpoint;
    audio_endpoint_t capture_endpoint;
    snd_pcm_uframes_t period_frames;
    size_t frame_bytes;
    uint8_t input_channels;
    uint32_t input_sample_rate;
    uint32_t input_resample_pos_q16;
    int16_t input_last_mono_sample;
    uint8_t input_have_last_sample;
    uint32_t input_probe_ticks;
    int16_t input_ring[AUDIO_INPUT_RING_FRAMES];
    uint32_t input_ring_head;
    uint32_t input_ring_tail;
    uint32_t input_ring_count;
    audio_control_state_t control;
    struct wav_voice {
        int16_t *pcm;
        size_t frames;
        uint32_t sample_rate;
        uint16_t channels;
    } wav[AUDIO_MAX_VOICES];
    char voice_name[AUDIO_MAX_VOICES][AUDIO_MAX_VOICE_NAME];
    int16_t block_buffer[BUFFER_FRAMES * 2];
    size_t pending_offset;
    size_t pending_bytes;
    int runtime_suspended;
    int warm_up_ticks;
    int waiting_for_host;
    uint32_t wait_notice_ticks;
    uint32_t playback_retry_ticks;
    uint32_t playback_err_notice_ticks;
} audio_ctx_t;

#endif
