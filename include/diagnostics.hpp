#pragma once

#include <cstddef>
#include <cstdint>

namespace audiox::diagnostics {

constexpr size_t kRuntimeTailBytes = 1024 * 1024;
constexpr size_t kKernelLogBytes = 256 * 1024;

int preserveTail(const char *path, size_t limit);

class BoundedLog {
public:
    BoundedLog() = default;
    BoundedLog(const BoundedLog &) = delete;
    BoundedLog &operator=(const BoundedLog &) = delete;
    ~BoundedLog();
    int open(const char *path, size_t limit);
    int append(const char *data, size_t size);
    int flush();
    void close();

private:
    int fd_ = -1;
    char path_[512] = {};
    size_t limit_ = 0;
    size_t size_ = 0;
    bool dirty_ = false;
};

int collectPersistentCrashes(const char *directory, const char *destination);
void pollKernelLog();

} // namespace audiox::diagnostics
