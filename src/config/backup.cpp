#include "config/backup.hpp"
#include "update_archive.hpp"
#include "watchdog_heartbeat.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace audiox {
namespace backup {
namespace {

// .axb v1: AXBACKUP, LE u32 version/count; entries: LE u32 path length/type,
// u64 size, path bytes, file bytes. Type 1=file, 2=directory. A final LE u32
// CRC32 covers every preceding byte, including the header and paths.
const char *const roots[] = {
    "config.txt", "config.staging.txt", "routing.txt", "midi_map.txt",
    "fx_params.txt", "volumes.txt", "bluetooth", "sfx",
};
constexpr size_t rootCount = sizeof(roots) / sizeof(roots[0]);
constexpr uint64_t configMax = 1024 * 1024;
constexpr const char *pendingName = ".config-restore";
constexpr const char *doneName = ".config-restore.done";

struct Error {
    char *text;
    size_t size;
    int fail(const char *format, ...) {
        va_list args;
        va_start(args, format);
        vsnprintf(text, size, format, args);
        va_end(args);
        return -1;
    }
    int io(const char *operation) { return fail("%s: %s", operation, strerror(errno)); }
};

struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
};

bool pathJoin(char *out, const char *a, const char *b) {
    int n = snprintf(out, maxPath, "%s/%s", a, b);
    if (n <= 0 || (size_t)n >= maxPath) {
        errno = ENAMETOOLONG;
        return false;
    }
    return true;
}

int syncDir(const char *path, Error &error) {
    Fd fd(open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW));
    if (fd.value < 0 || fsync(fd.value) < 0) return error.io("sync backup directory");
    return 0;
}

int writeAll(int fd, const void *data, size_t size, Error &error) {
    const char *p = (const char *)data;
    while (size) {
        audioxWatchdogHeartbeat();
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) errno = EIO;
        if (n <= 0) return error.io("write backup");
        p += n;
        size -= (size_t)n;
    }
    return 0;
}

int removeTree(const char *path) {
    struct stat st;
    if (lstat(path, &st) < 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path);
    DIR *dir = opendir(path);
    if (!dir) return -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) rc = -1;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        audioxWatchdogHeartbeat();
        char child[maxPath];
        if (!pathJoin(child, path, entry->d_name) || removeTree(child) < 0) {
            rc = -1;
            break;
        }
    }
    int saved = errno;
    closedir(dir);
    errno = saved;
    return rc < 0 ? rc : rmdir(path);
}

struct TempDir {
    char path[maxPath] = {};
    ~TempDir() {
        if (path[0] && removeTree(path) < 0) {
            printf("[CONFIG] [WARN] backup cleanup %s failed: %s\n", path, strerror(errno));
        }
    }
    int make(const char *root, Error &error) {
        if (!pathJoin(path, root, ".config-backup-XXXXXX")) return error.io("backup path");
        if (!mkdtemp(path)) {
            path[0] = '\0';
            return error.io("create backup workspace");
        }
        return 0;
    }
};

int entryRoot(const char *path) {
    for (size_t i = 0; i < rootCount; ++i) {
        size_t n = strlen(roots[i]);
        if (!strcmp(path, roots[i]) || (i >= 6 && !strncmp(path, roots[i], n) && path[n] == '/')) {
            return (int)i;
        }
    }
    return -1;
}

bool validPath(const char *path) {
    size_t n = strlen(path);
    if (!n || n >= maxPath || path[0] == '/' || path[n - 1] == '/') return false;
    unsigned depth = 0;
    const char *part = path;
    for (const char *p = path;; ++p) {
        if (*p == '/' || !*p) {
            size_t len = (size_t)(p - part);
            if (!len || (len == 1 && part[0] == '.') ||
                (len == 2 && !memcmp(part, "..", 2)) || ++depth > 16) return false;
            part = p + 1;
        } else if ((unsigned char)*p < 32 || *p == '\\' || *p == 127) {
            return false;
        }
        if (!*p) break;
    }
    int index = entryRoot(path);
    return index >= 0 && (index != 6 || !strcmp(path, "bluetooth") || !strcmp(path, "bluetooth/paired.txt"));
}

int checksum(int fd, uint64_t size, uint32_t *out, Error &error) {
    unsigned char buf[65536];
    uint32_t crc = 0;
    uint64_t pos = 0;
    while (pos < size) {
        audioxWatchdogHeartbeat();
        size_t n = size - pos > sizeof(buf) ? sizeof(buf) : (size_t)(size - pos);
        if (audioxUpdateReadAt(fd, buf, n, (off_t)pos) < 0) return error.io("read backup checksum");
        crc = audioxUpdateCrc32(crc, buf, n);
        pos += n;
    }
    *out = crc;
    return 0;
}

struct Writer {
    int fd;
    Error &error;
    uint32_t count = 0;
    uint64_t total = 16;

    int add(const char *root, const char *relative) {
        if (!validPath(relative)) return error.fail("Unsupported backup path: %s", relative);
        char path[maxPath];
        if (!pathJoin(path, root, relative)) return error.io("backup path");
        struct stat st;
        bool missingDirectory = false;
        if (lstat(path, &st) < 0) {
            if (errno != ENOENT) return error.io("inspect backup file");
            if (!strcmp(relative, "sfx") || !strcmp(relative, "bluetooth")) {
                st = {};
                st.st_mode = S_IFDIR;
                missingDirectory = true;
            } else if (strchr(relative, '/') && strcmp(relative, "bluetooth/paired.txt")) {
                return error.fail("Sound file disappeared during backup; retry: %s", relative);
            } else {
                return 0;
            }
        }
        bool directory = S_ISDIR(st.st_mode);
        if (!directory && !S_ISREG(st.st_mode)) return error.fail("Backup refuses links or special files: %s", relative);
        int index = entryRoot(relative);
        bool top = !strcmp(relative, roots[index]);
        if ((directory && (index < 6 || (index == 6 && !top))) || (!directory && index >= 6 && top)) {
            return error.fail("Unexpected file type: %s", relative);
        }
        uint64_t size = directory ? 0 : (uint64_t)st.st_size;
        if (index < 7 && size > configMax) return error.fail("Configuration file exceeds 1 MiB: %s", relative);
        size_t len = strlen(relative);
        if (++count > maxEntries || total + 20 + len > maxArchiveSize ||
            size > maxArchiveSize - total - 20 - len) return error.fail("Backup exceeds archive limits");
        unsigned char header[16];
        audioxUpdatePutU32(header, (uint32_t)len);
        audioxUpdatePutU32(header + 4, directory ? 2 : 1);
        audioxUpdatePutU64(header + 8, size);
        if (writeAll(fd, header, sizeof(header), error) < 0 || writeAll(fd, relative, len, error) < 0) return -1;
        total += sizeof(header) + len + size;
        if (!directory) {
            Fd in(open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK));
            if (in.value < 0) return error.io("open backup file");
            struct stat opened;
            if (fstat(in.value, &opened) < 0) return error.io("inspect open backup file");
            if (!S_ISREG(opened.st_mode) || opened.st_ino != st.st_ino || opened.st_size != st.st_size) {
                return error.fail("File changed during backup; retry: %s", relative);
            }
            char buf[65536];
            uint64_t pos = 0;
            while (pos < size) {
                size_t n = size - pos > sizeof(buf) ? sizeof(buf) : (size_t)(size - pos);
                if (audioxUpdateReadAt(in.value, buf, n, (off_t)pos) < 0) return error.io("read backup file");
                if (writeAll(fd, buf, n, error) < 0) return -1;
                pos += n;
            }
            struct stat after;
            if (fstat(in.value, &after) < 0) return error.io("recheck backup file");
            if (after.st_size != st.st_size || after.st_mtim.tv_sec != st.st_mtim.tv_sec ||
                after.st_mtim.tv_nsec != st.st_mtim.tv_nsec) return error.fail("File changed during backup; retry: %s", relative);
            return 0;
        }
        if (!strcmp(relative, "bluetooth")) return add(root, "bluetooth/paired.txt");
        if (missingDirectory) return 0;
        DIR *dir = opendir(path);
        if (!dir) return error.io("open sound directory");
        int rc = 0;
        for (;;) {
            errno = 0;
            dirent *entry = readdir(dir);
            if (!entry) {
                if (errno) rc = error.io("read sound directory");
                break;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[maxPath];
            if (!pathJoin(child, relative, entry->d_name)) { rc = error.io("sound path"); break; }
            if (add(root, child) < 0) { rc = -1; break; }
        }
        closedir(dir);
        return rc;
    }
};

int exists(const char *path, Error &error) {
    struct stat st;
    if (lstat(path, &st) == 0) return 1;
    return errno == ENOENT ? 0 : error.io("inspect restore target");
}

int discardUnpublished(const char *root, Error &error) {
    DIR *dir = opendir(root);
    if (!dir) return error.io("open configuration directory");
    int rc = 0;
    constexpr const char *prefix = ".config-backup-";
    size_t prefixLen = strlen(prefix);
    for (;;) {
        errno = 0;
        dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) rc = error.io("read configuration directory");
            break;
        }
        if (strncmp(entry->d_name, prefix, prefixLen) || strlen(entry->d_name) != prefixLen + 6) continue;
        char path[maxPath];
        if (!pathJoin(path, root, entry->d_name) || removeTree(path) < 0) {
            rc = error.io("clean interrupted backup staging");
            break;
        }
    }
    closedir(dir);
    return rc;
}

} // namespace

int create(const char *root, int archiveFd, char *text, size_t textSize) {
    Error error{text, textSize};
    char config[maxPath];
    if (!pathJoin(config, root, roots[0])) return error.io("backup path");
    if (access(config, R_OK) < 0) return error.io("read main configuration");
    if (ftruncate(archiveFd, 0) < 0 || lseek(archiveFd, 0, SEEK_SET) < 0) return error.io("initialize backup");
    unsigned char header[16] = {};
    memcpy(header, "AXBACKUP", 8);
    audioxUpdatePutU32(header + 8, 1);
    if (writeAll(archiveFd, header, sizeof(header), error) < 0) return -1;
    Writer writer{archiveFd, error};
    for (const char *name : roots) if (writer.add(root, name) < 0) return -1;
    audioxUpdatePutU32(header + 12, writer.count);
    if (pwrite(archiveFd, header, sizeof(header), 0) != (ssize_t)sizeof(header)) return error.io("finalize backup header");
    uint32_t crc;
    if (checksum(archiveFd, writer.total, &crc, error) < 0) return -1;
    unsigned char trailer[4];
    audioxUpdatePutU32(trailer, crc);
    if (writeAll(archiveFd, trailer, sizeof(trailer), error) < 0 || fsync(archiveFd) < 0) return error.io("finalize backup");
    return 0;
}

int stage(const char *root, int fd, uint64_t size, char *text, size_t textSize) {
    Error error{text, textSize};
    if (size < 20 || size > maxArchiveSize) return error.fail("Invalid backup size (maximum 4 GiB)");
    unsigned char header[16], trailer[4];
    if (audioxUpdateReadAt(fd, header, sizeof(header), 0) < 0 ||
        audioxUpdateReadAt(fd, trailer, 4, (off_t)(size - 4)) < 0) return error.io("read backup header");
    if (memcmp(header, "AXBACKUP", 8) || audioxUpdateGetU32(header + 8) != 1) return error.fail("Unsupported AudioX backup format or version");
    uint32_t count = audioxUpdateGetU32(header + 12), crc;
    if (!count || count > maxEntries) return error.fail("Invalid backup entry count");
    if (checksum(fd, size - 4, &crc, error) < 0) return -1;
    if (crc != audioxUpdateGetU32(trailer)) return error.fail("Backup checksum mismatch; no settings changed");
    char pending[maxPath];
    if (!pathJoin(pending, root, pendingName)) return error.io("restore path");
    int present = exists(pending, error);
    if (present < 0) return -1;
    if (present) return error.fail("A restore is already staged; reboot the device first");
    TempDir temp;
    if (temp.make(root, error) < 0) return -1;
    char fresh[maxPath];
    if (!pathJoin(fresh, temp.path, "new") || mkdir(fresh, 0700) < 0) return error.io("create restore directory");
    uint64_t pos = 16;
    uint32_t mask = 0;
    for (uint32_t i = 0; i < count; ++i) {
        audioxWatchdogHeartbeat();
        if (pos > size - 4 || size - 4 - pos < 16) return error.fail("Truncated backup entry");
        if (audioxUpdateReadAt(fd, header, 16, (off_t)pos) < 0) return error.io("read backup entry");
        pos += 16;
        uint32_t len = audioxUpdateGetU32(header), type = audioxUpdateGetU32(header + 4);
        uint64_t bytes = audioxUpdateGetU64(header + 8);
        if (!len || len >= maxPath || len > size - 4 - pos ||
            bytes > size - 4 - pos - len || (type != 1 && type != 2) || (type == 2 && bytes)) {
            return error.fail("Invalid backup entry size or type");
        }
        char name[maxPath];
        if (audioxUpdateReadAt(fd, name, len, (off_t)pos) < 0) return error.io("read backup path");
        pos += len;
        name[len] = '\0';
        if (memchr(name, '\0', len) || !validPath(name)) return error.fail("Backup contains an unsafe or unsupported path");
        int index = entryRoot(name);
        bool top = !strcmp(name, roots[index]);
        if ((index < 6 && (type != 1 || bytes > configMax)) ||
            (index == 6 && !top && (type != 1 || bytes > configMax)) ||
            (index >= 6 && top && type != 2)) return error.fail("Invalid configuration entry type or size");
        char path[maxPath];
        if (!pathJoin(path, fresh, name)) return error.io("restore file path");
        if (type == 2) {
            if (mkdir(path, 0700) < 0) return error.io("create restore directory (duplicate or missing parent)");
        } else {
            Fd out(open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600));
            if (out.value < 0) return error.io("create restore file (duplicate or missing parent)");
            char buf[65536];
            uint64_t copied = 0;
            while (copied < bytes) {
                size_t n = bytes - copied > sizeof(buf) ? sizeof(buf) : (size_t)(bytes - copied);
                if (audioxUpdateReadAt(fd, buf, n, (off_t)(pos + copied)) < 0) return error.io("read backup payload");
                if (writeAll(out.value, buf, n, error) < 0) return -1;
                copied += n;
            }
            if (fsync(out.value) < 0) return error.io("sync restored file");
        }
        if (top) mask |= 1u << index;
        pos += bytes;
    }
    if (pos != size - 4 || !(mask & 1) || !(mask & (1u << 6)) || !(mask & (1u << 7))) {
        return error.fail("Backup is incomplete or contains trailing data");
    }
    char plan[maxPath], old[maxPath];
    if (!pathJoin(plan, temp.path, "plan") || !pathJoin(old, temp.path, "old")) return error.io("restore plan path");
    Fd planFd(open(plan, O_WRONLY | O_CREAT | O_EXCL, 0600));
    unsigned char encoded[4];
    audioxUpdatePutU32(encoded, mask);
    if (planFd.value < 0 || writeAll(planFd.value, encoded, 4, error) < 0 ||
        fsync(planFd.value) < 0 || mkdir(old, 0700) < 0) return error.io("write restore plan");
    // syncfs also persists nested sound directories before publishing the pending restore.
    if (syncfs(planFd.value) < 0) return error.io("sync staged restore");
    if (rename(temp.path, pending) < 0) return error.io("publish staged restore");
    temp.path[0] = '\0';
    return syncDir(root, error);
}

int applyPending(const char *root, char *text, size_t textSize) {
    Error error{text, textSize};
    if (discardUnpublished(root, error) < 0) return -1;
    char pending[maxPath], done[maxPath];
    if (!pathJoin(pending, root, pendingName) || !pathJoin(done, root, doneName)) return error.io("restore path");
    if (removeTree(done) < 0) return error.io("clean completed restore");
    int present = exists(pending, error);
    if (present <= 0) return present;
    char plan[maxPath], fresh[maxPath], old[maxPath];
    if (!pathJoin(plan, pending, "plan") || !pathJoin(fresh, pending, "new") || !pathJoin(old, pending, "old")) return error.io("restore plan path");
    Fd fd(open(plan, O_RDONLY | O_NOFOLLOW));
    unsigned char encoded[4];
    if (fd.value < 0 || audioxUpdateReadAt(fd.value, encoded, 4, 0) < 0) return error.io("read restore plan");
    uint32_t mask = audioxUpdateGetU32(encoded);
    if (mask >= (1u << rootCount) || !(mask & 1) || !(mask & (1u << 6)) || !(mask & (1u << 7))) return error.fail("Invalid staged restore plan");
    printf("[CONFIG] applying staged configuration and soundboard backup\n");
    for (size_t i = 0; i < rootCount; ++i) {
        audioxWatchdogHeartbeat();
        char src[maxPath], dst[maxPath], saved[maxPath];
        if (!pathJoin(src, fresh, roots[i]) || !pathJoin(dst, root, roots[i]) || !pathJoin(saved, old, roots[i])) return error.io("restore target path");
        int hasNew = exists(src, error), hasOld = exists(saved, error), hasTarget = exists(dst, error);
        if (hasNew < 0 || hasOld < 0 || hasTarget < 0) return -1;
        if (!hasNew && (mask & (1u << i))) {
            // A previous boot already installed this entry.
            if (!hasTarget) return error.fail("Interrupted restore is missing %s", roots[i]);
            continue;
        }
        if (hasTarget) {
            if (hasOld) return error.fail("Conflicting interrupted restore target: %s", roots[i]);
            if (rename(dst, saved) < 0 || syncDir(old, error) < 0 || syncDir(root, error) < 0) return error.io("save pre-restore configuration");
        }
        if (hasNew && (rename(src, dst) < 0 || syncDir(root, error) < 0 || syncDir(fresh, error) < 0)) return error.io("install restored configuration");
    }
    // Retire the transaction atomically before deleting its old files.
    if (rename(pending, done) < 0 || syncDir(root, error) < 0) return error.io("finish configuration restore");
    if (removeTree(done) < 0) printf("[CONFIG] [WARN] restored successfully, but old backup cleanup failed: %s\n", strerror(errno));
    printf("[CONFIG] backup restored successfully\n");
    return 0;
}

} // namespace backup
} // namespace audiox
