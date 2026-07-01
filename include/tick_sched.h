#ifndef TICK_SCHED_H
#define TICK_SCHED_H

typedef struct tick_task {
    int interval;
    int ticks;
} tick_task_t;

static inline void tick_task_init(tick_task_t *task, int interval) {
    if (!task) {
        return;
    }

    task->interval = (interval > 0) ? interval : 1;
    task->ticks = 0;
}

static inline void tick_task_reset(tick_task_t *task) {
    if (!task) {
        return;
    }
    task->ticks = 0;
}

static inline int tick_task_due(tick_task_t *task) {
    if (!task) {
        return 0;
    }

    if (++task->ticks >= task->interval) {
        task->ticks = 0;
        return 1;
    }
    return 0;
}

#endif
