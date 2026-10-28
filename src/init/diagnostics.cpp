#include "diagnostics.hpp"
#include "defs.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

namespace audiox::diagnostics {

static int writeAll(int fd, const char *data, size_t size) {
    while (size) {
        ssize_t written = write(fd, data, size);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return written < 0 ? -errno : -EIO;
        data += written;
        size -= static_cast<size_t>(written);
    }
    return 0;
}

int preserveTail(const char *path, size_t limit) {
    int input = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (input < 0) return errno == ENOENT ? 0 : -errno;
    struct stat status = {};
    int rc = fstat(input, &status) < 0 ? -errno : 0;
    if (rc == 0 && !S_ISREG(status.st_mode)) rc = -EINVAL;
    if (rc == 0 && status.st_size == 0) {
        ::close(input);
        return 0;
    }
    char previous[544], temporary[552];
    if (snprintf(previous, sizeof(previous), "%s.previous", path) >= static_cast<int>(sizeof(previous)) ||
        snprintf(temporary, sizeof(temporary), "%s.tmp", previous) >= static_cast<int>(sizeof(temporary))) {
        rc = -ENAMETOOLONG;
    }
    if (rc == 0 && status.st_size > static_cast<off_t>(limit) &&
        lseek(input, status.st_size - static_cast<off_t>(limit), SEEK_SET) < 0) rc = -errno;
    int output = -1;
    if (rc == 0) {
        output = ::open(temporary, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (output < 0) rc = -errno;
    }
    char buffer[8192];
    size_t remaining = limit;
    while (rc == 0 && remaining) {
        ssize_t count = read(input, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { rc = -errno; break; }
        if (count == 0) break;
        rc = writeAll(output, buffer, static_cast<size_t>(count));
        remaining -= static_cast<size_t>(count);
    }
    if (rc == 0 && fsync(output) < 0) rc = -errno;
    if (output >= 0 && ::close(output) < 0 && rc == 0) rc = -errno;
    ::close(input);
    if (rc == 0 && rename(temporary, previous) < 0) rc = -errno;
    if (rc < 0 && output >= 0) unlink(temporary);
    return rc;
}

BoundedLog::~BoundedLog() {
    close();
}

int BoundedLog::open(const char *path, size_t limit) {
    close();
    if (limit == 0 || strlen(path) >= sizeof(path_)) return -EINVAL;
    int rc = preserveTail(path, limit);
    if (rc < 0) return rc;
    fd_ = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd_ < 0) return -errno;
    snprintf(path_, sizeof(path_), "%s", path);
    limit_ = limit;
    size_ = 0;
    return 0;
}

int BoundedLog::append(const char *data, size_t size) {
    if (fd_ < 0) return -EBADF;
    if (size > limit_) {
        data += size - limit_;
        size = limit_;
    }
    if (size_ + size > limit_) {
        char path[sizeof(path_)];
        snprintf(path, sizeof(path), "%s", path_);
        int rc = open(path, limit_);
        if (rc < 0) return rc;
    }
    int rc = writeAll(fd_, data, size);
    if (rc == 0) {
        size_ += size;
        dirty_ = true;
    }
    return rc;
}

int BoundedLog::flush() {
    if (fd_ < 0) return -EBADF;
    if (dirty_ && fdatasync(fd_) < 0) return -errno;
    dirty_ = false;
    return 0;
}

void BoundedLog::close() {
    if (fd_ >= 0 && ::close(fd_) < 0) {
        fprintf(stderr, "[INIT] [WARN] closing diagnostic log: %s\n", strerror(errno));
    }
    fd_ = -1;
    dirty_ = false;
}

int collectPersistentCrashes(const char *directory, const char *destination) {
    DIR *dir = opendir(directory);
    if (!dir) return -errno;
    BoundedLog output;
    int rc = 0;
    size_t remaining = kKernelLogBytes;
    bool opened = false;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(dir);
        if (!entry) {
            if (errno) rc = -errno;
            break;
        }
        if (entry->d_name[0] == '.') continue;
        int fd = openat(dirfd(dir), entry->d_name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        if (fd < 0) { rc = -errno; break; }
        struct stat status = {};
        if (fstat(fd, &status) < 0) {
            rc = -errno;
        } else if (S_ISREG(status.st_mode)) {
            if (!opened) {
                rc = output.open(destination, kKernelLogBytes);
                opened = rc == 0;
            }
            char buffer[8192];
            int n = snprintf(buffer, sizeof(buffer), "\n--- %s ---\n", entry->d_name);
            if (rc == 0 && remaining >= static_cast<size_t>(n)) {
                rc = output.append(buffer, static_cast<size_t>(n));
                remaining -= static_cast<size_t>(n);
            }
            while (rc == 0 && remaining) {
                ssize_t count = read(fd, buffer, remaining < sizeof(buffer) ? remaining : sizeof(buffer));
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) { rc = -errno; break; }
                if (count == 0) break;
                rc = output.append(buffer, static_cast<size_t>(count));
                remaining -= static_cast<size_t>(count);
            }
        }
        if (::close(fd) < 0 && rc == 0) rc = -errno;
        if (rc < 0 || remaining == 0) break;
    }
    closedir(dir);
    if (rc == 0 && opened) rc = output.flush();
    return rc;
}

static int drainKernelMessages(int input, BoundedLog &output, unsigned *overruns) {
    char record[8192];
    for (unsigned i = 0; i < 32; ++i) {
        ssize_t count = read(input, record, sizeof(record));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && errno == EAGAIN) return 0;
        if (count < 0 && errno == EPIPE) {
            if (((*overruns)++ % 256) == 0) printf("[INIT] [WARN] kernel message capture overrun; some records lost\n");
            continue;
        }
        if (count < 0) return -errno;
        if (count == 0) return 0;
        int rc = output.append(record, static_cast<size_t>(count));
        if (rc < 0) return rc;
    }
    return 0;
}

void pollKernelLog() {
    static int input = -1;
    static bool initialized = false;
    static BoundedLog output;
    static uint64_t nextPoll = 0, nextFlush = 0;
    static unsigned overruns = 0;
    static bool clockWarning = false;
    timespec time = {};
    if (clock_gettime(CLOCK_MONOTONIC, &time) < 0) {
        if (!clockWarning) printf("[INIT] [WARN] kernel capture clock unavailable: %s\n", strerror(errno));
        clockWarning = true;
        return;
    }
    uint64_t now = static_cast<uint64_t>(time.tv_sec) * 1000 + time.tv_nsec / 1000000;
    if (now < nextPoll) return;
    nextPoll = now + 100;
    int rc = 0;
    if (!initialized) {
        initialized = true;
        input = ::open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (input < 0) {
            printf("[INIT] [WARN] kernel message capture unavailable: %s\n", strerror(errno));
            return;
        }
        rc = output.open(ROOT_MOUNT_POINT "/kernel.log", kKernelLogBytes);
        if (rc == 0) {
            utsname info = {};
            char boot[80] = "unknown";
            int bootFd = ::open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC);
            if (bootFd >= 0) {
                ssize_t count = read(bootFd, boot, sizeof(boot) - 1);
                if (count > 0) boot[count] = '\0';
                else printf("[INIT] [WARN] cannot read boot ID\n");
                ::close(bootFd);
            } else {
                printf("[INIT] [WARN] boot ID unavailable: %s\n", strerror(errno));
            }
            if (uname(&info) < 0) printf("[INIT] [WARN] uname: %s\n", strerror(errno));
            char header[512];
#ifndef AUDIOX_VERSION_STRING
#define AUDIOX_VERSION_STRING "unknown"
#endif
#ifndef AUDIOX_BOARD
#define AUDIOX_BOARD "unknown"
#endif
            int n = snprintf(header, sizeof(header), "audiox=%s kernel=%s board=%s machine=%s boot=%s\n",
                             AUDIOX_VERSION_STRING, info.release, AUDIOX_BOARD, info.machine, boot);
            if (n < 0 || static_cast<size_t>(n) >= sizeof(header)) {
                rc = -EOVERFLOW;
            } else {
                printf("[INIT] [INFO] %s", header);
                rc = output.append(header, static_cast<size_t>(n));
            }
        }
        nextFlush = now + 10000;
    }
    if (input < 0) return;
    if (rc == 0) rc = drainKernelMessages(input, output, &overruns);
    if (rc == 0 && now >= nextFlush) {
        rc = output.flush();
        nextFlush = now + 10000;
    }
    if (rc < 0) {
        printf("[INIT] [WARN] kernel message capture stopped: %s\n", strerror(-rc));
        ::close(input);
        input = -1;
        output.close();
    }
}

} // namespace audiox::diagnostics
