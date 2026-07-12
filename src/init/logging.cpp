#include "init.hpp"
#include "context.hpp"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

int setupLogging(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }
    if (remove("/audiox/stdout.log") != 0) {
        if (errno != ENOENT) {
            printf("[INIT] [WARN] failed to remove old /audiox/stdout.log: %s\n", strerror(errno));
        }
    }
    if (remove("/audiox/stderr.log") != 0) {
        if (errno != ENOENT) {
            printf("[INIT] [WARN] failed to remove old /audiox/stderr.log: %s\n", strerror(errno));
        }
    }
    freopen("/audiox/stdout.log", "a", stdout);
    freopen("/audiox/stderr.log", "a", stderr);

    // How can I mark it so that the log is always flushed to disk? I want to make sure that if the system crashes, the log is not lost. I think I can use setvbuf() to set the buffer mode to line buffered or unbuffered. Let's try line buffered first.
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    return RET_OK;
}

void flushLogs() {
    fflush(stdout);
    fflush(stderr);
    fsync(fileno(stdout));
    fsync(fileno(stderr));
}