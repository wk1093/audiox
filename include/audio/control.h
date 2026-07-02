#ifndef AUDIO_CONTROL_H
#define AUDIO_CONTROL_H

#include "audio/types.h"
#include "audio/voice_bank.h"

static inline void audio_refresh_enabled(audio_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    ctx->enabled = 0;
    for (int i = 0; i < audio_voice_count(ctx); ++i) {
        if (ctx->voice_enabled[i]) {
            ctx->enabled = 1;
            break;
        }
    }
}

static inline int audio_voice_is_enabled(const audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return 0;
    }

    int enabled = 0;
    pthread_mutex_lock((pthread_mutex_t *)&ctx->state_lock);
    enabled = ctx->voice_enabled[voice_index] ? 1 : 0;
    pthread_mutex_unlock((pthread_mutex_t *)&ctx->state_lock);
    return enabled;
}

static inline void audio_set_voice(audio_ctx_t *ctx, int voice_index, int enabled) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (enabled && !ctx->voice_enabled[voice_index]) {
        ctx->voice_cursor_q16[voice_index] = 0;
    }
    ctx->voice_enabled[voice_index] = enabled ? 1 : 0;
    if (!enabled) {
        ctx->voice_gain_q15[voice_index] = 0;
    }
    audio_refresh_enabled(ctx);
    pthread_mutex_unlock(&ctx->state_lock);
}

static inline int audio_toggle_voice(audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return 0;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (!ctx->voice_enabled[voice_index]) {
        ctx->voice_cursor_q16[voice_index] = 0;
    }
    ctx->voice_enabled[voice_index] = ctx->voice_enabled[voice_index] ? 0 : 1;
    if (!ctx->voice_enabled[voice_index]) {
        ctx->voice_gain_q15[voice_index] = 0;
    }
    audio_refresh_enabled(ctx);
    int enabled = ctx->voice_enabled[voice_index] ? 1 : 0;
    pthread_mutex_unlock(&ctx->state_lock);
    return enabled;
}

static inline int audio_trigger_voice(audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return -1;
    }

    pthread_mutex_lock(&ctx->state_lock);
    ctx->voice_cursor_q16[voice_index] = 0;
    ctx->voice_enabled[voice_index] = 1;
    ctx->voice_loop[voice_index] = 0;
    ctx->voice_gain_q15[voice_index] = 0;
    audio_refresh_enabled(ctx);
    pthread_mutex_unlock(&ctx->state_lock);
    return 0;
}

#endif
