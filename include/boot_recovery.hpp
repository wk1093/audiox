#pragma once

// Update rollback state shared by the bootloader, the PID 1 watchdog and the runtime.
//
// State lives in <config root>/system:
//   boot-backup/           last confirmed-good boot files plus a "version" file
//   boot-trial             "attempts=N" while a newly installed boot set is unconfirmed
//   boot-trial.log         per-attempt events (bootloader attempts, watchdog failure reasons)
//   last-boot-failure.txt  report written when the bootloader reverts to the backup

#include "defs.hpp"
#include "update_archive.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BOOT_RECOVERY_DIR_NAME "system"
#define BOOT_RECOVERY_BACKUP_NAME "boot-backup"
#define BOOT_RECOVERY_TRIAL_NAME "boot-trial"
#define BOOT_RECOVERY_EVENTS_NAME "boot-trial.log"
#define BOOT_RECOVERY_REPORT_NAME "last-boot-failure.txt"
#define BOOT_RECOVERY_VERSION_NAME "version"
#define BOOT_RECOVERY_TRIAL_ENV "AUDIOX_BOOT_TRIAL"
#define BOOT_RECOVERY_STATE_DIR ROOT_MOUNT_POINT "/" BOOT_RECOVERY_DIR_NAME
#define BOOT_RECOVERY_TRIAL_PATH BOOT_RECOVERY_STATE_DIR "/" BOOT_RECOVERY_TRIAL_NAME
#define BOOT_RECOVERY_EVENTS_PATH BOOT_RECOVERY_STATE_DIR "/" BOOT_RECOVERY_EVENTS_NAME
#define BOOT_RECOVERY_REPORT_PATH BOOT_RECOVERY_STATE_DIR "/" BOOT_RECOVERY_REPORT_NAME
#define AUDIOX_VERSION_FILE "/etc/audiox-version"

#ifndef BOOT_RECOVERY_MAX_ATTEMPTS
#define BOOT_RECOVERY_MAX_ATTEMPTS 3
#endif
#ifndef BOOT_RECOVERY_CONFIRM_SECONDS
#define BOOT_RECOVERY_CONFIRM_SECONDS 60
#endif
#define BOOT_RECOVERY_LOG_TAIL_BYTES (32 * 1024)

enum BootTrialAction {
    BOOT_TRIAL_NONE,
    BOOT_TRIAL_ATTEMPT,
    BOOT_TRIAL_REVERT,
};

static inline const char *bootRecoveryFileName(size_t index) {
    static const char *const names[] = {
        "initramfs.cpio.gz",
        "program.cpio.gz",
        "config.txt",
    };
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : NULL;
}

static inline bool bootRecoveryIsBootFile(const char *name) {
    for (size_t i = 0; bootRecoveryFileName(i); ++i) {
        if (strcmp(name, bootRecoveryFileName(i)) == 0) {
            return true;
        }
    }
    return audioxUpdateValidKernelName(name);
}

#define BOOT_RECOVERY_MAX_FILES 8

// Boot files present in a backup directory, ordered so config.txt (which selects the kernel) is last.
static inline size_t bootRecoveryListBackup(const char *backup_dir, char names[BOOT_RECOVERY_MAX_FILES][AUDIOX_UPDATE_VALUE_MAX]) {
    size_t count = 0;
    bool has_config = false;
    DIR *d = opendir(backup_dir);
    if (!d) {
        return 0;
    }
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (!bootRecoveryIsBootFile(entry->d_name) || strlen(entry->d_name) >= AUDIOX_UPDATE_VALUE_MAX) {
            continue;
        }
        if (strcmp(entry->d_name, "config.txt") == 0) {
            has_config = true;
            continue;
        }
        if (count < BOOT_RECOVERY_MAX_FILES - 1) {
            snprintf(names[count++], AUDIOX_UPDATE_VALUE_MAX, "%s", entry->d_name);
        }
    }
    closedir(d);
    if (has_config) {
        snprintf(names[count++], AUDIOX_UPDATE_VALUE_MAX, "%s", "config.txt");
    }
    return count;
}

static inline int brPath(char *out, size_t out_sz, const char *a, const char *b, const char *c) {
    int n = c ? snprintf(out, out_sz, "%s/%s/%s", a, b, c) : snprintf(out, out_sz, "%s/%s", a, b);
    return (n > 0 && (size_t)n < out_sz) ? 0 : -1;
}

static inline int brWriteAll(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len > 0) {
        ssize_t nw = write(fd, p, len);
        if (nw < 0 && errno == EINTR) {
            continue;
        }
        if (nw <= 0) {
            return -1;
        }
        p += nw;
        len -= (size_t)nw;
    }
    return 0;
}

static inline void brFsyncDir(const char *path) {
    int fd = open(path, O_RDONLY | O_DIRECTORY);
    if (fd >= 0) {
        (void)fsync(fd);
        close(fd);
    }
}

static inline int brCopyFile(const char *src, const char *dst) {
    int in = open(src, O_RDONLY);
    if (in < 0) {
        return -1;
    }
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) {
        int saved = errno;
        close(in);
        errno = saved;
        return -1;
    }
    char buf[16384];
    int rc = 0;
    for (;;) {
        ssize_t nr = read(in, buf, sizeof(buf));
        if (nr < 0 && errno == EINTR) {
            continue;
        }
        if (nr < 0 || (nr > 0 && brWriteAll(out, buf, (size_t)nr) < 0)) {
            rc = -1;
            break;
        }
        if (nr == 0) {
            break;
        }
    }
    if (rc == 0 && fsync(out) < 0) {
        rc = -1;
    }
    int saved = errno;
    close(out);
    close(in);
    if (rc < 0) {
        unlink(dst);
        errno = saved;
    }
    return rc;
}

static inline int brWriteFileAtomic(const char *dir, const char *name, const char *text) {
    char path[512];
    char tmp[512];
    if (brPath(path, sizeof(path), dir, name, NULL) < 0 ||
        snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return -1;
    }
    if (brWriteAll(fd, text, strlen(text)) < 0 || fsync(fd) < 0) {
        int saved = errno;
        close(fd);
        unlink(tmp);
        errno = saved;
        return -1;
    }
    close(fd);
    if (rename(tmp, path) < 0) {
        int saved = errno;
        unlink(tmp);
        errno = saved;
        return -1;
    }
    brFsyncDir(dir);
    return 0;
}

static inline ssize_t brReadSmallFile(const char *path, char *buf, size_t buf_sz) {
    if (buf_sz == 0) {
        return -1;
    }
    buf[0] = '\0';
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }
    size_t have = 0;
    while (have + 1 < buf_sz) {
        ssize_t nr = read(fd, buf + have, buf_sz - 1 - have);
        if (nr < 0 && errno == EINTR) {
            continue;
        }
        if (nr <= 0) {
            break;
        }
        have += (size_t)nr;
    }
    close(fd);
    buf[have] = '\0';
    return (ssize_t)have;
}

static inline void brAppendText(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) {
        return;
    }
    (void)brWriteAll(fd, text, strlen(text));
    (void)fsync(fd);
    close(fd);
}

static inline void brRemoveFlatDir(const char *dir) {
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
                continue;
            }
            char path[512];
            if (brPath(path, sizeof(path), dir, entry->d_name, NULL) == 0) {
                unlink(path);
            }
        }
        closedir(d);
    }
    rmdir(dir);
}

static inline int bootRecoveryEnsureStateDir(const char *root_dir, char *state_dir, size_t state_sz) {
    if (brPath(state_dir, state_sz, root_dir, BOOT_RECOVERY_DIR_NAME, NULL) < 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(state_dir, 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static inline int bootRecoveryHasBackup(const char *root_dir) {
    char version_path[512];
    if (brPath(version_path, sizeof(version_path), root_dir, BOOT_RECOVERY_DIR_NAME "/" BOOT_RECOVERY_BACKUP_NAME,
               BOOT_RECOVERY_VERSION_NAME) < 0) {
        return 0;
    }
    return access(version_path, F_OK) == 0;
}

static inline int bootRecoveryTrialPending(const char *root_dir) {
    char trial_path[512];
    if (brPath(trial_path, sizeof(trial_path), root_dir, BOOT_RECOVERY_DIR_NAME, BOOT_RECOVERY_TRIAL_NAME) < 0) {
        return 0;
    }
    return access(trial_path, F_OK) == 0;
}

// Called by a running image before it modifies any boot file. An existing backup is kept because
// it is the last confirmed-good boot set; otherwise the current boot files are copied first.
static inline int bootRecoveryPrepareUpdate(const char *root_dir, const char *boot_dir, const char *version_text) {
    char state_dir[512];
    if (bootRecoveryEnsureStateDir(root_dir, state_dir, sizeof(state_dir)) < 0) {
        return -1;
    }

    if (!bootRecoveryHasBackup(root_dir)) {
        char backup_dir[512];
        char tmp_dir[512];
        if (brPath(backup_dir, sizeof(backup_dir), state_dir, BOOT_RECOVERY_BACKUP_NAME, NULL) < 0 ||
            brPath(tmp_dir, sizeof(tmp_dir), state_dir, BOOT_RECOVERY_BACKUP_NAME ".tmp", NULL) < 0) {
            errno = ENAMETOOLONG;
            return -1;
        }
        brRemoveFlatDir(tmp_dir);
        brRemoveFlatDir(backup_dir);
        if (mkdir(tmp_dir, 0755) < 0) {
            return -1;
        }
        char config_path[512];
        char config_text[16384];
        char kernel_name[AUDIOX_UPDATE_VALUE_MAX];
        ssize_t config_len = -1;
        if (brPath(config_path, sizeof(config_path), boot_dir, "config.txt", NULL) == 0) {
            config_len = brReadSmallFile(config_path, config_text, sizeof(config_text));
        }
        audioxConfigKernelName(config_text, config_len > 0 ? (size_t)config_len : 0, kernel_name, sizeof(kernel_name));
        const char *backup_names[] = {bootRecoveryFileName(0), bootRecoveryFileName(1), bootRecoveryFileName(2), kernel_name};
        for (const char *name : backup_names) {
            char src[512];
            char dst[512];
            if (brPath(src, sizeof(src), boot_dir, name, NULL) < 0 ||
                brPath(dst, sizeof(dst), tmp_dir, name, NULL) < 0) {
                errno = ENAMETOOLONG;
                return -1;
            }
            if (access(src, F_OK) < 0) {
                continue;
            }
            if (brCopyFile(src, dst) < 0) {
                int saved = errno;
                brRemoveFlatDir(tmp_dir);
                errno = saved;
                return -1;
            }
        }
        if (brWriteFileAtomic(tmp_dir, BOOT_RECOVERY_VERSION_NAME, version_text && version_text[0] ? version_text : "unknown\n") < 0) {
            int saved = errno;
            brRemoveFlatDir(tmp_dir);
            errno = saved;
            return -1;
        }
        if (rename(tmp_dir, backup_dir) < 0) {
            int saved = errno;
            brRemoveFlatDir(tmp_dir);
            errno = saved;
            return -1;
        }
        brFsyncDir(state_dir);
    }

    char events_path[512];
    if (brPath(events_path, sizeof(events_path), state_dir, BOOT_RECOVERY_EVENTS_NAME, NULL) == 0) {
        unlink(events_path);
    }
    return brWriteFileAtomic(state_dir, BOOT_RECOVERY_TRIAL_NAME, "attempts=0\n");
}

// Called by the bootloader on every boot that is not installing a staged runtime.
static inline BootTrialAction bootRecoveryBeginBoot(const char *root_dir, const char *version_text, unsigned *attempts_out) {
    char state_dir[512];
    char trial_path[512];
    if (brPath(state_dir, sizeof(state_dir), root_dir, BOOT_RECOVERY_DIR_NAME, NULL) < 0 ||
        brPath(trial_path, sizeof(trial_path), state_dir, BOOT_RECOVERY_TRIAL_NAME, NULL) < 0) {
        return BOOT_TRIAL_NONE;
    }

    char text[64];
    if (brReadSmallFile(trial_path, text, sizeof(text)) < 0) {
        return BOOT_TRIAL_NONE;
    }

    // An unreadable counter is treated as exhausted so a corrupt state file cannot loop forever.
    unsigned attempts = BOOT_RECOVERY_MAX_ATTEMPTS;
    unsigned parsed = 0;
    if (sscanf(text, "attempts=%u", &parsed) == 1) {
        attempts = parsed;
    }
    if (attempts_out) {
        *attempts_out = attempts;
    }
    if (attempts >= BOOT_RECOVERY_MAX_ATTEMPTS) {
        return BOOT_TRIAL_REVERT;
    }

    ++attempts;
    char next[64];
    snprintf(next, sizeof(next), "attempts=%u\n", attempts);
    if (brWriteFileAtomic(state_dir, BOOT_RECOVERY_TRIAL_NAME, next) < 0) {
        return BOOT_TRIAL_REVERT;
    }
    if (attempts_out) {
        *attempts_out = attempts;
    }

    char events_path[512];
    char line[256];
    if (brPath(events_path, sizeof(events_path), state_dir, BOOT_RECOVERY_EVENTS_NAME, NULL) == 0) {
        snprintf(line, sizeof(line), "[BOOT] trial attempt %u/%u, runtime version %s%s",
                 attempts, (unsigned)BOOT_RECOVERY_MAX_ATTEMPTS,
                 version_text && version_text[0] ? version_text : "unknown",
                 version_text && version_text[0] && version_text[strlen(version_text) - 1] == '\n' ? "" : "\n");
        brAppendText(events_path, line);
    }
    return BOOT_TRIAL_ATTEMPT;
}

static inline void brReportAppendFile(FILE *report, const char *title, const char *path, size_t tail_bytes) {
    fprintf(report, "\n--- %s ---\n", title);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(report, "(unavailable: %s)\n", strerror(errno));
        return;
    }
    struct stat st;
    if (tail_bytes > 0 && fstat(fd, &st) == 0 && st.st_size > (off_t)tail_bytes) {
        fprintf(report, "(last %zu of %lld bytes)\n", tail_bytes, (long long)st.st_size);
        (void)lseek(fd, st.st_size - (off_t)tail_bytes, SEEK_SET);
    }
    char buf[4096];
    ssize_t nr;
    while ((nr = read(fd, buf, sizeof(buf))) > 0) {
        fwrite(buf, 1, (size_t)nr, report);
    }
    close(fd);
}

static inline void brTrimLine(char *text) {
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[--len] = '\0';
    }
}

// Writes the failure report, restores the backup boot set and clears the trial state.
// The caller must sync and reboot afterwards so the firmware loads the restored files.
static inline int bootRecoveryRevert(const char *root_dir, const char *boot_dir, const char *failed_version, unsigned attempts) {
    char state_dir[512];
    char backup_dir[512];
    if (bootRecoveryEnsureStateDir(root_dir, state_dir, sizeof(state_dir)) < 0 ||
        brPath(backup_dir, sizeof(backup_dir), state_dir, BOOT_RECOVERY_BACKUP_NAME, NULL) < 0) {
        return -1;
    }

    const int has_backup = bootRecoveryHasBackup(root_dir);
    char restore_names[BOOT_RECOVERY_MAX_FILES][AUDIOX_UPDATE_VALUE_MAX];
    const size_t restore_count = has_backup ? bootRecoveryListBackup(backup_dir, restore_names) : 0;
    char restored_version[128] = "none";
    if (has_backup) {
        char version_path[512];
        if (brPath(version_path, sizeof(version_path), backup_dir, BOOT_RECOVERY_VERSION_NAME, NULL) == 0) {
            (void)brReadSmallFile(version_path, restored_version, sizeof(restored_version));
            brTrimLine(restored_version);
        }
    }
    char failed[128];
    snprintf(failed, sizeof(failed), "%s", failed_version && failed_version[0] ? failed_version : "unknown");
    brTrimLine(failed);

    char report_path[512];
    char report_tmp[512];
    if (brPath(report_path, sizeof(report_path), state_dir, BOOT_RECOVERY_REPORT_NAME, NULL) < 0 ||
        snprintf(report_tmp, sizeof(report_tmp), "%s.tmp", report_path) >= (int)sizeof(report_tmp)) {
        return -1;
    }

    // The report is written before restoring so a power loss mid-restore still leaves evidence.
    FILE *report = fopen(report_tmp, "w");
    if (report) {
        fprintf(report, "failed_version=%s\n", failed);
        fprintf(report, "restored_version=%s\n", has_backup ? restored_version : "none");
        fprintf(report, "attempts=%u\n", attempts);
        fprintf(report, "reverted=%s\n", has_backup ? "yes" : "no");
        fprintf(report, "restored_files=");
        for (size_t i = 0; i < restore_count; ++i) {
            fprintf(report, "%s%s", i == 0 ? "" : ",", restore_names[i]);
        }
        fprintf(report, "\n\nThe updated image failed to stay healthy for %u boot attempt(s).\n", attempts);
        if (!has_backup) {
            fprintf(report, "No backup boot set was available, so the failing image was left in place.\n");
        }
        char path[512];
        if (brPath(path, sizeof(path), state_dir, BOOT_RECOVERY_EVENTS_NAME, NULL) == 0) {
            brReportAppendFile(report, "boot trial events", path, BOOT_RECOVERY_LOG_TAIL_BYTES);
        }
        if (brPath(path, sizeof(path), root_dir, "stdout.log", NULL) == 0) {
            brReportAppendFile(report, "stdout.log from the last failed attempt", path, BOOT_RECOVERY_LOG_TAIL_BYTES);
        }
        if (brPath(path, sizeof(path), root_dir, "stderr.log", NULL) == 0) {
            brReportAppendFile(report, "stderr.log from the last failed attempt", path, BOOT_RECOVERY_LOG_TAIL_BYTES);
        }
        fflush(report);
        (void)fsync(fileno(report));
        fclose(report);
        if (rename(report_tmp, report_path) < 0) {
            unlink(report_tmp);
        }
        brFsyncDir(state_dir);
    }

    int rc = 0;
    for (size_t i = 0; i < restore_count; ++i) {
        char src[512];
        char dst[512];
        char tmp[512];
        if (brPath(src, sizeof(src), backup_dir, restore_names[i], NULL) < 0 ||
            brPath(dst, sizeof(dst), boot_dir, restore_names[i], NULL) < 0 ||
            snprintf(tmp, sizeof(tmp), "%s.restore", dst) >= (int)sizeof(tmp)) {
            rc = -1;
            continue;
        }
        if (access(src, F_OK) < 0) {
            continue;
        }
        if (brCopyFile(src, tmp) < 0 || rename(tmp, dst) < 0) {
            unlink(tmp);
            rc = -1;
        }
    }
    if (rc < 0) {
        // Keep the trial and backup so the next boot retries the restore.
        return -1;
    }

    char path[512];
    if (brPath(path, sizeof(path), state_dir, BOOT_RECOVERY_TRIAL_NAME, NULL) == 0) {
        unlink(path);
    }
    if (brPath(path, sizeof(path), state_dir, BOOT_RECOVERY_EVENTS_NAME, NULL) == 0) {
        unlink(path);
    }
    brRemoveFlatDir(backup_dir);
    brFsyncDir(state_dir);
    return has_backup ? 0 : 1;
}

// Called by the runtime once a trial boot has stayed healthy; drops the safety copy.
static inline void bootRecoveryConfirm(const char *root_dir) {
    char state_dir[512];
    char path[512];
    if (brPath(state_dir, sizeof(state_dir), root_dir, BOOT_RECOVERY_DIR_NAME, NULL) < 0) {
        return;
    }
    if (brPath(path, sizeof(path), state_dir, BOOT_RECOVERY_TRIAL_NAME, NULL) == 0) {
        unlink(path);
    }
    if (brPath(path, sizeof(path), state_dir, BOOT_RECOVERY_EVENTS_NAME, NULL) == 0) {
        unlink(path);
    }
    if (brPath(path, sizeof(path), state_dir, BOOT_RECOVERY_BACKUP_NAME, NULL) == 0) {
        brRemoveFlatDir(path);
    }
    brFsyncDir(state_dir);
}
