#ifndef AUDIO_CONTROL_H
#define AUDIO_CONTROL_H

#include "audio/types.h"
#include "audio/voice_bank.h"

static inline void audio_refresh_enabled(audio_ctx_t *ctx) {
    if (!ctx) {
        return;
    }

    ctx->control.enabled = 0;
    for (int i = 0; i < audio_voice_count(ctx); ++i) {
        if (ctx->control.voice_enabled[i]) {
            ctx->control.enabled = 1;
            break;
        }
    }
}

static inline void audio_copy_control_snapshot(const audio_ctx_t *ctx,
                                               audio_control_snapshot_t *snapshot) {
    if (!ctx || !snapshot) {
        return;
    }

    pthread_mutex_lock((pthread_mutex_t *)&ctx->state_lock);
    snapshot->enabled = ctx->control.enabled;
    snapshot->voice_count = ctx->control.voice_count;
    snapshot->voice_note_base = ctx->control.voice_note_base;
    memcpy(snapshot->voice_enabled,
           ctx->control.voice_enabled,
           sizeof(snapshot->voice_enabled));
    pthread_mutex_unlock((pthread_mutex_t *)&ctx->state_lock);
}

static inline int audio_voice_is_enabled(const audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return 0;
    }

    int enabled = 0;
    pthread_mutex_lock((pthread_mutex_t *)&ctx->state_lock);
    enabled = ctx->control.voice_enabled[voice_index] ? 1 : 0;
    pthread_mutex_unlock((pthread_mutex_t *)&ctx->state_lock);
    return enabled;
}

static inline void audio_set_voice(audio_ctx_t *ctx, int voice_index, int enabled) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (enabled && !ctx->control.voice_enabled[voice_index]) {
        ctx->control.voice_cursor_q16[voice_index] = 0;
    }
    ctx->control.voice_enabled[voice_index] = enabled ? 1 : 0;
    if (!enabled) {
        ctx->control.voice_gain_q15[voice_index] = 0;
    }
    audio_refresh_enabled(ctx);
    pthread_mutex_unlock(&ctx->state_lock);
}

static inline int audio_toggle_voice(audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return 0;
    }

    pthread_mutex_lock(&ctx->state_lock);
    if (!ctx->control.voice_enabled[voice_index]) {
        ctx->control.voice_cursor_q16[voice_index] = 0;
    }
    ctx->control.voice_enabled[voice_index] = ctx->control.voice_enabled[voice_index] ? 0 : 1;
    if (!ctx->control.voice_enabled[voice_index]) {
        ctx->control.voice_gain_q15[voice_index] = 0;
    }
    audio_refresh_enabled(ctx);
    int enabled = ctx->control.voice_enabled[voice_index] ? 1 : 0;
    pthread_mutex_unlock(&ctx->state_lock);
    return enabled;
}

static inline int audio_trigger_voice(audio_ctx_t *ctx, int voice_index) {
    if (!ctx || voice_index < 0 || voice_index >= audio_voice_count(ctx)) {
        return -1;
    }

    pthread_mutex_lock(&ctx->state_lock);
    ctx->control.voice_cursor_q16[voice_index] = 0;
    ctx->control.voice_enabled[voice_index] = 1;
    ctx->control.voice_loop[voice_index] = 0;
    ctx->control.voice_gain_q15[voice_index] = 0;
    audio_refresh_enabled(ctx);
    pthread_mutex_unlock(&ctx->state_lock);
    return 0;
}

#endif
