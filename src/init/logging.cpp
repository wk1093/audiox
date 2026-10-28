#include "init.hpp"
#include "context.hpp"
#include "diagnostics.hpp"

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/mount.h>
#include <sys/stat.h>

int setupLogging(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }
    const char *paths[] = {ROOT_MOUNT_POINT "/stdout.log", ROOT_MOUNT_POINT "/stderr.log"};
    for (const char *path : paths) {
        int rc = audiox::diagnostics::preserveTail(path, audiox::diagnostics::kRuntimeTailBytes);
        if (rc < 0) {
            printf("[INIT] [WARN] preserving %s failed: %s; leaving existing logs intact\n", path, strerror(-rc));
            return RET_WARN;
        }
    }
    FILE *res = freopen(ROOT_MOUNT_POINT "/stdout.log", "w", stdout);
    if (!res) {
        printf("[INIT] [WARN] failed to redirect stdout to /audiox/stdout.log: %s\n", strerror(errno));
        return RET_WARN;
    }
    res = freopen(ROOT_MOUNT_POINT "/stderr.log", "w", stderr);
    if (!res) {
        printf("[INIT] [WARN] failed to redirect stderr to /audiox/stderr.log: %s\n", strerror(errno));
        return RET_WARN;
    }


    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (mkdir("/sys/fs/pstore", 0755) < 0 && errno != EEXIST) {
        printf("[INIT] [INFO] pstore unavailable: %s; persistent panic capture not enabled\n", strerror(errno));
    } else if (mount("pstore", "/sys/fs/pstore", "pstore", 0, nullptr) < 0 && errno != EBUSY) {
        printf("[INIT] [INFO] pstore unavailable: %s; persistent panic capture not enabled\n", strerror(errno));
    } else {
        int rc = audiox::diagnostics::collectPersistentCrashes("/sys/fs/pstore", ROOT_MOUNT_POINT "/pstore.log");
        if (rc < 0) printf("[INIT] [WARN] pstore collection failed: %s\n", strerror(-rc));
        else printf("[INIT] [INFO] pstore records checked (no backend or reserved memory configured by AudioX)\n");
    }
    audiox::diagnostics::pollKernelLog();

    return RET_OK;
}

void flushLogs() {
    fflush(stdout);
    fflush(stderr);
    fsync(fileno(stdout));
    fsync(fileno(stderr));
}