#ifndef RT_SCHED_H
#define RT_SCHED_H

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>

static inline int rt_set_current_thread_other(const char *tag) {
    struct sched_param param;
    memset(&param, 0, sizeof(param));
    if (sched_setscheduler(0, SCHED_OTHER, &param) < 0) {
        printf("[INIT] [WARN] Failed to set %s thread to SCHED_OTHER: %s\n", tag, strerror(errno));
        return -1;
    }
    return 0;
}

static inline int rt_create_fifo_thread(pthread_t *thread,
                                        void *(*entry)(void *),
                                        void *arg,
                                        int priority) {
    pthread_attr_t attr;
    if (pthread_attr_init(&attr) != 0) {
        printf("[INIT] [WARN] pthread_attr_init failed for audio thread.\n");
        return pthread_create(thread, NULL, entry, arg);
    }

    int attr_ok = 1;
    if (pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED) != 0) {
        attr_ok = 0;
    }
    if (attr_ok && pthread_attr_setschedpolicy(&attr, SCHED_FIFO) != 0) {
        attr_ok = 0;
    }

    struct sched_param param;
    memset(&param, 0, sizeof(param));
    param.sched_priority = priority;
    if (attr_ok && pthread_attr_setschedparam(&attr, &param) != 0) {
        attr_ok = 0;
    }

    int rc = pthread_create(thread, attr_ok ? &attr : NULL, entry, arg);
    pthread_attr_destroy(&attr);
    return rc;
}

#endif
