#include "diagnostics.hpp"

#include <cassert>
#include <cerrno>
#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>
#include <sys/syscall.h>
#include "../src/init/diagnostics.cpp"

namespace fs = std::filesystem;
static int writeMode = 0;
static unsigned kernelReads = 0;
static int kernelReadError = 0;

extern "C" ssize_t read(int fd, void *buffer, size_t count) {
    if (fd == -42) {
        ++kernelReads;
        if (kernelReadError) {
            errno = kernelReadError;
            return -1;
        }
        assert(count >= 7);
        memcpy(buffer, "record\n", 7);
        return 7;
    }
    return syscall(SYS_read, fd, buffer, count);
}

extern "C" ssize_t write(int fd, const void *buffer, size_t count) {
    if (writeMode == 1) {
        writeMode = 2;
        errno = EINTR;
        return -1;
    }
    if (writeMode == 2) {
        writeMode = 0;
        count = count > 2 ? 2 : count;
    }
    if (writeMode == 3) {
        errno = ENOSPC;
        return -1;
    }
    return syscall(SYS_write, fd, buffer, count);
}

static std::string readFile(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(file), {});
}

static void put(const fs::path &path, const std::string &text) {
    std::ofstream file(path);
    file << text;
    assert(file.good());
}

int main() {
    char temporary[] = "/tmp/audiox-diagnostics-test-XXXXXX";
    assert(mkdtemp(temporary));
    fs::path root(temporary);
    const std::string path = (root / "kernel.log").string();
    using namespace audiox::diagnostics;
    assert(preserveTail(path.c_str(), 8) == 0);
    put(path, "old boot:abcdefgh");
    assert(preserveTail(path.c_str(), 8) == 0);
    assert(readFile(path + ".previous") == "abcdefgh");
    assert(readFile(path) == "old boot:abcdefgh");
    {
        BoundedLog log;
        assert(log.open(path.c_str(), 8) == 0);
        assert(log.append("new boot", 8) == 0);
        assert(log.flush() == 0);
        assert(readFile(path + ".previous") == "abcdefgh");
        assert(log.append("runtime", 7) == 0);
        assert(log.flush() == 0);
        assert(readFile(path + ".previous") == "new boot");
        assert(readFile(path) == "runtime");
        assert(log.append("0123456789", 10) == 0);
        assert(log.flush() == 0);
        assert(readFile(path) == "23456789");
        assert(readFile(path + ".previous") == "runtime");
        writeMode = 1;
        assert(log.append("partial", 7) == 0);
        assert(log.flush() == 0);
        assert(readFile(path) == "partial");
        writeMode = 3;
        assert(log.append("x", 1) == -ENOSPC);
        writeMode = 0;
    }
    put(path, "");
    assert(preserveTail(path.c_str(), 8) == 0);
    assert(readFile(path + ".previous") == "23456789");
    assert(preserveTail(root.c_str(), 8) == -EINVAL);
    {
        BoundedLog log;
        assert(log.open((root / "missing/log").c_str(), 8) == -ENOENT);
        assert(log.append("x", 1) == -EBADF);
        assert(log.flush() == -EBADF);
    }
    fs::create_directory(root / "pstore");
    std::string crash = (root / "pstore.log").string();
    put(crash, "older crash");
    assert(collectPersistentCrashes((root / "pstore").c_str(), crash.c_str()) == 0);
    assert(readFile(crash) == "older crash");
    put(root / "pstore/dmesg-ramoops-0", std::string(kKernelLogBytes * 2, 'x'));
    assert(collectPersistentCrashes((root / "pstore").c_str(), crash.c_str()) == 0);
    assert(fs::file_size(crash) == kKernelLogBytes);
    assert(readFile(crash).find("dmesg-ramoops-0") != std::string::npos);
    assert(readFile(crash + ".previous") == "older crash");
    assert(fs::exists(root / "pstore/dmesg-ramoops-0"));
    assert(collectPersistentCrashes((root / "absent").c_str(), crash.c_str()) == -ENOENT);
    // A symlink or unwritable destination must report an error, not erase old evidence.
    fs::create_symlink(root / "absent", root / "link");
    assert(preserveTail((root / "link").c_str(), 8) == -ELOOP);
    fs::create_directory(root / "blocked.previous");
    put(root / "blocked", "evidence");
    assert(preserveTail((root / "blocked").c_str(), 8) < 0);
    assert(readFile(root / "blocked") == "evidence");
    assert(!fs::exists(root / "blocked.previous.tmp"));
    {
        BoundedLog log;
        assert(log.open((root / "records").c_str(), kKernelLogBytes) == 0);
        unsigned overruns = 0;
        assert(drainKernelMessages(-42, log, &overruns) == 0);
        assert(kernelReads == 32);
        assert(log.flush() == 0);
        assert(fs::file_size(root / "records") == 32 * 7);
        kernelReadError = EPIPE;
        assert(drainKernelMessages(-42, log, &overruns) == 0);
        assert(overruns == 32 && kernelReads == 64);
        kernelReadError = EAGAIN;
        assert(drainKernelMessages(-42, log, &overruns) == 0);
        assert(kernelReads == 65);
        kernelReadError = EINTR;
        assert(drainKernelMessages(-42, log, &overruns) == 0);
        assert(kernelReads == 97);
        kernelReadError = ENODEV;
        assert(drainKernelMessages(-42, log, &overruns) == -ENODEV);
        kernelReadError = 0;
        writeMode = 3;
        assert(drainKernelMessages(-42, log, &overruns) == -ENOSPC);
        writeMode = 0;
    }
    fs::remove_all(root);
    puts("diagnostic retention, rotation, bounds and failure tests passed");
}
