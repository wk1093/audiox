#pragma once

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace audiox {
class AtomicConfigFile {
    std::string path_, temporary_;
    FILE *file_ = nullptr;
public:
    explicit AtomicConfigFile(const char *path) : path_(path), temporary_(path_ + ".tmp.XXXXXX") {
        int fd = mkstemp(temporary_.data());
        if (fd < 0) { temporary_.clear(); return; }
        file_ = fdopen(fd, "w");
        if (!file_) { int saved = errno; close(fd); unlink(temporary_.c_str()); errno = saved; }
    }
    ~AtomicConfigFile() {
        if (file_) fclose(file_);
        if (!temporary_.empty() && unlink(temporary_.c_str()) < 0 && errno != ENOENT)
            fprintf(stderr, "[CONFIG] [WARN] temporary file cleanup failed: %s\n", strerror(errno));
    }
    FILE *get() const { return file_; }
    int commit() {
        if (!file_) return -1;
        bool ok = !ferror(file_) && fflush(file_) == 0 && fsync(fileno(file_)) == 0;
        int saved = errno;
        if (fclose(file_) < 0 && ok) { ok = false; saved = errno; }
        file_ = nullptr;
        if (!ok) { errno = saved; return -1; }
        if (rename(temporary_.c_str(), path_.c_str()) < 0) return -1;
        temporary_.clear();
        const size_t slash = path_.find_last_of('/');
        std::string parent = slash == std::string::npos ? "." :
                             slash == 0 ? "/" : path_.substr(0, slash);
        int dir = open(parent.c_str(), O_RDONLY | O_DIRECTORY);
        if (dir < 0) return -1;
        int rc = fsync(dir);
        saved = errno;
        if (close(dir) < 0 && rc == 0) return -1;
        errno = saved;
        return rc;
    }
    AtomicConfigFile(const AtomicConfigFile &) = delete;
    AtomicConfigFile &operator=(const AtomicConfigFile &) = delete;
};
}
