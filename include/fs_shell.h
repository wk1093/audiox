#ifndef FS_SHELL_H
#define FS_SHELL_H

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static inline void fs_shell_help(void) {
    printf("\n");
    printf("Temporary Initramfs FS Shell\n");
    printf("  help                 Show this help\n");
    printf("  pwd                  Print current directory\n");
    printf("  cd <dir>             Change directory\n");
    printf("  ls [dir]             List directory entries\n");
    printf("  mounts               Show mounted filesystems\n");
    printf("  cat <file>           Print text file\n");
    printf("  boot | continue      Exit shell and continue normal init\n");
    printf("  halt                 Stop here forever\n");
    printf("\n");
}

static inline int fs_shell_ls(const char *path) {
    static const int ls_linecap = 8;

    const char *target = (path && path[0] != '\0') ? path : ".";
    DIR *dir = opendir(target);
    if (!dir) {
        printf("[FS-SHELL] ls failed for '%s': %s\n", target, strerror(errno));
        return -1;
    }

    struct dirent *entry = NULL;
    int i = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        if (entry->d_type == DT_DIR) {
            printf("%s/ ", entry->d_name);
            if (++i % ls_linecap == 0) printf("\n");
        } else {
            printf("%s ", entry->d_name);
            if (++i % ls_linecap == 0) printf("\n");
        }
    }

    closedir(dir);
    return 0;
}

static inline int fs_shell_mounts(void) {
    FILE *fp = fopen("/proc/mounts", "r");
    if (!fp) {
        printf("[FS-SHELL] could not open /proc/mounts: %s\n", strerror(errno));
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char src[160];
        char dst[160];
        char fstype[64];
        if (sscanf(line, "%159s %159s %63s", src, dst, fstype) == 3) {
            printf("%-24s %-24s %s\n", src, dst, fstype);
        }
    }

    fclose(fp);
    return 0;
}

static inline int fs_shell_cat(const char *path) {
    if (!path || path[0] == '\0') {
        printf("[FS-SHELL] usage: cat <file>\n");
        return -1;
    }

    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("[FS-SHELL] cat failed for '%s': %s\n", path, strerror(errno));
        return -1;
    }

    char line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        fputs(line, stdout);
    }
    fclose(fp);
    return 0;
}

static inline void run_temp_fs_shell(void) {
#if DEBUG_SHELL
    if (!isatty(STDIN_FILENO)) {
        printf("[INIT] stdin is not a tty; skipping temporary FS shell.\n");
        return;
    }

    printf("\n[INIT] Entering temporary filesystem shell. Type 'boot' to continue init.\n");
    fs_shell_help();

    char cwd[PATH_MAX];
    char line[512];

    while (1) {
        if (!getcwd(cwd, sizeof(cwd))) {
            snprintf(cwd, sizeof(cwd), "?");
        }
        printf("fs:%s# ", cwd);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {
            printf("\n[INIT] EOF on shell input, continuing init.\n");
            break;
        }

        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) {
            line[len - 1] = '\0';
            --len;
        }

        char *cmd = strtok(line, " \t");
        if (!cmd) {
            continue;
        }

        char *arg = strtok(NULL, "");
        if (arg) {
            while (*arg == ' ' || *arg == '\t') {
                ++arg;
            }
        }

        if (strcmp(cmd, "help") == 0) {
            fs_shell_help();
            continue;
        }

        if (strcmp(cmd, "pwd") == 0) {
            if (getcwd(cwd, sizeof(cwd))) {
                printf("%s\n", cwd);
            } else {
                printf("[FS-SHELL] pwd failed: %s\n", strerror(errno));
            }
            continue;
        }

        if (strcmp(cmd, "cd") == 0) {
            const char *target = (arg && arg[0] != '\0') ? arg : "/";
            if (chdir(target) < 0) {
                printf("[FS-SHELL] cd failed for '%s': %s\n", target, strerror(errno));
            }
            continue;
        }

        if (strcmp(cmd, "ls") == 0) {
            (void)fs_shell_ls(arg);
            continue;
        }

        if (strcmp(cmd, "mounts") == 0) {
            (void)fs_shell_mounts();
            continue;
        }

        if (strcmp(cmd, "cat") == 0) {
            (void)fs_shell_cat(arg);
            continue;
        }

        if (strcmp(cmd, "boot") == 0 || strcmp(cmd, "continue") == 0) {
            break;
        }

        if (strcmp(cmd, "halt") == 0) {
            printf("[INIT] Halt requested from FS shell.\n");
            while (1) {
                sleep(60);
            }
        }

        printf("[FS-SHELL] unknown command '%s' (type 'help')\n", cmd);
    }

    if (chdir("/") < 0) {
        printf("[INIT] [WARN] Failed to restore cwd to '/': %s\n", strerror(errno));
    }
    printf("[INIT] Leaving temporary filesystem shell.\n\n");
#else
    printf("[INIT] DEBUG_SHELL disabled; skipping temporary filesystem shell.\n");
#endif
}

#endif
