#ifndef MIDI_RUNTIME_H
#define MIDI_RUNTIME_H

#include "midi_input.h"
#include "tick_sched.h"

typedef struct midi_runtime {
    midi_ctx_t ctx;
    tick_task_t probe_tick;
} midi_runtime_t;

static inline void midi_runtime_init(midi_runtime_t *rt, int disconnected_probe_interval_ticks) {
    if (!rt) {
        return;
    }

    midi_ctx_init(&rt->ctx);
    tick_task_init(&rt->probe_tick, disconnected_probe_interval_ticks);
}

static inline int midi_runtime_poll(midi_runtime_t *rt,
                                    midi_log_push_fn log_push,
                                    void *log_ctx,
                                    int *connection_changed) {
    if (!rt) {
        if (connection_changed) {
            *connection_changed = 0;
        }
        return 0;
    }

    if (connection_changed) {
        *connection_changed = 0;
    }

    if (rt->ctx.fd < 0 && !tick_task_due(&rt->probe_tick)) {
        return 0;
    }

    int changed = 0;
    int updated = midi_poll(&rt->ctx, log_push, log_ctx, &changed);
    if (connection_changed) {
        *connection_changed = changed;
    }

    if (changed) {
        tick_task_reset(&rt->probe_tick);
    }
    return updated;
}

static inline void midi_runtime_disconnect(midi_runtime_t *rt,
                                           midi_log_push_fn log_push,
                                           void *log_ctx) {
    if (!rt) {
        return;
    }

    midi_disconnect(&rt->ctx, log_push, log_ctx);
}

#endif
