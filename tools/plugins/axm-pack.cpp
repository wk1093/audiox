#include "audio/plugin_package.hpp"
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
bool readInput(const char *path, size_t max, std::vector<uint8_t> &data, std::string &error) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { error = std::string("cannot open ") + path; return false; }
    struct stat st{};
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) > max) {
        close(fd); error = std::string("invalid input file: ") + path; return false;
    }
    data.resize(static_cast<size_t>(st.st_size));
    size_t pos = 0;
    while (pos < data.size()) {
        ssize_t n = read(fd, data.data() + pos, data.size() - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); error = "cannot read input"; return false; }
        pos += static_cast<size_t>(n);
    }
    uint8_t extra;
    ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    close(fd);
    if (n != 0) { error = "input changed while reading"; return false; }
    return true;
}
bool writeOutput(const char *path, const std::vector<uint8_t> &data, std::string &error) {
    // Never truncate an existing file (including an input module or metadata file).
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
    if (fd < 0) { error = std::string("cannot create output: ") + std::strerror(errno); return false; }
    size_t pos = 0;
    bool ok = true;
    while (pos < data.size()) {
        ssize_t n = write(fd, data.data() + pos, data.size() - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        pos += static_cast<size_t>(n);
    }
    if (ok && fsync(fd) < 0) ok = false;
    if (close(fd) < 0) ok = false;
    if (!ok) { unlink(path); error = "cannot write output"; }
    return ok;
}
}

int main(int argc, char **argv) {
    using namespace audiox::plugin;
    if (argc < 4 || argc > 4 + static_cast<int>(kMaxAssets)) {
        std::fprintf(stderr, "usage: axm-pack axm.txt module.so output.axm [asset.js|asset.css|asset.html ...]\n");
        return 2;
    }
    std::vector<Entry> entries;
    std::string error;
    for (int i = 1; i < argc; ++i) {
        if (i == 3) continue;
        Entry entry;
        entry.kind = i == 1 ? Metadata : i == 2 ? Module : Asset;
        entry.name = i == 1 ? "axm.txt" : i == 2 ? "module.so" : argv[i];
        if (i > 3) {
            size_t slash = entry.name.find_last_of('/');
            if (slash != std::string::npos) entry.name.erase(0, slash + 1);
        }
        const size_t max = i == 1 ? kMaxMetadataBytes : i == 2 ? kMaxModuleBytes : kMaxAssetBytes;
        if (!readInput(argv[i], max, entry.data, error)) {
            std::fprintf(stderr, "axm-pack: %s\n", error.c_str()); return 1;
        }
        entries.push_back(std::move(entry));
    }
    std::vector<uint8_t> archive;
    if (!buildPackage(entries, archive, error) || !writeOutput(argv[3], archive, error)) {
        std::fprintf(stderr, "axm-pack: %s\n", error.c_str()); return 1;
    }
    std::printf("wrote %zu bytes to %s\n", archive.size(), argv[3]);
    return 0;
}
