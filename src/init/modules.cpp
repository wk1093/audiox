#include "init.hpp"

#include <fcntl.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <errno.h>
#include <string.h>
#include <stdio.h>

static inline int loadModule(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[INIT] [ERR] Failed to open module %s: %s\n", path, strerror(errno));
        return RET_ERR;
    }

    int rc = syscall(SYS_finit_module, fd, "", 0);
    int saved_errno = errno;
    close(fd);

    if (rc < 0 && saved_errno != EEXIST) {
        if (saved_errno == ENOENT) {
            printf("[INIT] [ERR] Syscall failed for %s: %s (errno %d, likely missing dependency module)\n", path, strerror(saved_errno), saved_errno);
        } else {
            printf("[INIT] [ERR] Syscall failed for %s: %s (errno %d)\n", path, strerror(saved_errno), saved_errno);
        }
        return RET_ERR;
    }
    return RET_OK;
}

static inline int loadModulesFromList(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("[INIT] [ERR] Could not open module load list %s\n", path);
        return RET_ERR;
    }

    char line[768];

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }

        if (*p == '\0' || *p == '\n' || *p == '#') {
            continue;
        }

        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[len - 1] = '\0';
            --len;
        }

        if (len == 0) {
            continue;
        }

        printf("  -> Loading: %s\n", p);
        int rc = loadModule(p);
        if (rc != RET_OK) {
            fclose(fp);
            return rc;
        }
    }

    fclose(fp);
    return RET_OK;
}

int loadBaseModules() {
    return loadModulesFromList(MODULE_LOAD_BASE_LIST_FILE);
}

int loadAllModules() {
    return loadModulesFromList(MODULE_LOAD_NORMAL_LIST_FILE);
}