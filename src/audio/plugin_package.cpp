#include "audio/plugin_package.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <limits>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace audiox::plugin {
namespace {
constexpr size_t kMaxEntries = 2 + kMaxAssets;
constexpr size_t kMaxDirectoryEntries = 64;
constexpr size_t kMaxEntryName = 63;

bool fail(std::string &error, const char *message) {
    errno = EINVAL;
    error = message;
    return false;
}
bool systemFail(std::string &error, const char *message) {
    const int saved = errno;
    error = std::string(message) + ": " + std::strerror(saved);
    errno = saved;
    return false;
}
struct Fd {
    int value;
    explicit Fd(int fd = -1) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;
};
bool lower(char c) { return c >= 'a' && c <= 'z'; }
bool digit(char c) { return c >= '0' && c <= '9'; }
bool identifier(const std::string &s, size_t max) {
    if (s.empty() || s.size() > max || !lower(s[0])) return false;
    for (char c : s) if (!lower(c) && !digit(c) && c != '_') return false;
    return true;
}
bool printable(const std::string &s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (unsigned char c : s) if (c < 32 || c > 126) return false;
    return true;
}
uint64_t le(const uint8_t *p, size_t n) {
    uint64_t v = 0;
    for (size_t i = 0; i < n; ++i) v |= uint64_t(p[i]) << (8 * i);
    return v;
}
void put(std::vector<uint8_t> &out, uint64_t v, size_t n) {
    for (size_t i = 0; i < n; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}
uint32_t crc(const uint8_t *p, size_t n) {
    uint32_t v = 0xffffffffu;
    for (size_t i = 0; i < n; ++i) {
        v ^= p[i];
        for (int b = 0; b < 8; ++b) v = (v >> 1) ^ (0xedb88320u & (0u - (v & 1u)));
    }
    return ~v;
}
bool floatValue(const std::string &s, float &v) {
    // Locale-independent decimal syntax: no whitespace, hex, NaN or infinity.
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    size_t digits = 0;
    while (i < s.size() && digit(s[i])) { ++i; ++digits; }
    if (i < s.size() && s[i] == '.') {
        ++i;
        while (i < s.size() && digit(s[i])) { ++i; ++digits; }
    }
    if (!digits) return false;
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        size_t start = i;
        while (i < s.size() && digit(s[i])) ++i;
        if (start == i) return false;
    }
    if (i != s.size()) return false;
    std::istringstream stream(s);
    stream.imbue(std::locale::classic());
    stream >> v;
    return !stream.fail() && std::isfinite(v);
}
bool elf(const std::vector<uint8_t> &data, bool native, std::string &error) {
    if (data.size() < 64 || std::memcmp(data.data(), ELFMAG, SELFMAG) ||
        data[EI_CLASS] != ELFCLASS64 || data[EI_DATA] != ELFDATA2LSB ||
        data[EI_VERSION] != EV_CURRENT || le(data.data() + 16, 2) != ET_DYN ||
        le(data.data() + 20, 4) != EV_CURRENT || le(data.data() + 52, 2) != 64)
        return fail(error, "module must be a little-endian ELF64 shared object");
    const uint64_t machine = le(data.data() + 18, 2);
    if (machine != EM_X86_64 && machine != EM_AARCH64)
        return fail(error, "unsupported module architecture");
#if defined(__x86_64__)
    constexpr uint64_t hostMachine = EM_X86_64;
#elif defined(__aarch64__)
    constexpr uint64_t hostMachine = EM_AARCH64;
#else
    constexpr uint64_t hostMachine = 0;
#endif
    if (native && machine != hostMachine) return fail(error, "module architecture does not match host");
    const uint64_t phoff = le(data.data() + 32, 8), shoff = le(data.data() + 40, 8);
    const uint64_t phsize = le(data.data() + 54, 2), phnum = le(data.data() + 56, 2);
    const uint64_t shsize = le(data.data() + 58, 2), shnum = le(data.data() + 60, 2);
    if (!phnum || phnum == PN_XNUM || phsize != 56 || phoff < 64 ||
        phoff > data.size() || phnum > (data.size() - phoff) / phsize)
        return fail(error, "invalid ELF program header bounds");
    if ((shnum && (shsize != 64 || shoff < 64 || shoff > data.size() ||
                   shnum > (data.size() - shoff) / shsize)) || (!shnum && shoff))
        return fail(error, "invalid ELF section header bounds");
    bool load = false;
    for (uint64_t i = 0; i < phnum; ++i) {
        const uint8_t *p = data.data() + phoff + i * phsize;
        uint64_t offset = le(p + 8, 8), bytes = le(p + 32, 8);
        if (offset > data.size() || bytes > data.size() - offset)
            return fail(error, "invalid ELF segment bounds");
        if (le(p, 4) == PT_LOAD) {
            if (le(p + 40, 8) < bytes) return fail(error, "invalid ELF load segment");
            load = true;
        }
    }
    if (!load) return fail(error, "ELF has no load segment");
    for (uint64_t i = 0; i < shnum; ++i) {
        const uint8_t *s = data.data() + shoff + i * shsize;
        uint64_t offset = le(s + 24, 8), bytes = le(s + 32, 8);
        if (le(s + 4, 4) != SHT_NOBITS &&
            (offset > data.size() || bytes > data.size() - offset))
            return fail(error, "invalid ELF section bounds");
    }
    return true;
}
int openRoot(const std::string &root, bool create, std::string &error) {
    if (root.empty() || root.size() > 4096) { fail(error, "invalid plugin root"); return -1; }
    size_t check = root[0] == '/' ? 1 : 0;
    while (check < root.size()) {
        size_t end = root.find('/', check);
        if (end == std::string::npos) end = root.size();
        const std::string part = root.substr(check, end - check);
        if (part.empty() || part == ".." || part.size() > 255) {
            fail(error, "invalid plugin root component"); return -1;
        }
        for (unsigned char c : part) {
            if (c < 32 || c == 127) {
                fail(error, "invalid plugin root component"); return -1;
            }
        }
        check = end + 1;
    }
    int current = open(root[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (current < 0) { systemFail(error, "open root anchor"); return -1; }
    size_t pos = root[0] == '/' ? 1 : 0;
    while (pos < root.size()) {
        size_t end = root.find('/', pos);
        if (end == std::string::npos) end = root.size();
        std::string part = root.substr(pos, end - pos);
        if (part.empty() || part == ".." || part.size() > 255) {
            close(current); fail(error, "invalid plugin root component"); return -1;
        }
        int next = openat(current, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            const bool created = mkdirat(current, part.c_str(), 0755) == 0;
            if (!created && errno != EEXIST) {
                int saved = errno; close(current); errno = saved;
                systemFail(error, "create plugin root"); return -1;
            }
            next = openat(current, part.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (next >= 0 && created && fchmod(next, 0755) < 0) {
                int saved = errno; close(next); close(current); errno = saved;
                systemFail(error, "set plugin root mode"); return -1;
            }
            if (next >= 0 && fsync(current) < 0) {
                int saved = errno; close(next); close(current); errno = saved;
                systemFail(error, "sync plugin root parent"); return -1;
            }
        }
        int saved = errno;
        close(current);
        if (next < 0) { errno = saved; systemFail(error, "open plugin root"); return -1; }
        current = next;
        pos = end + 1;
    }
    return current;
}
bool readFile(int dir, const std::string &name, size_t max, std::string &data, std::string &error) {
    Fd fd(openat(dir, name.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC));
    if (fd.value < 0) return systemFail(error, "open plugin file");
    struct stat st{};
    if (fstat(fd.value, &st) < 0) return systemFail(error, "stat plugin file");
    if (!S_ISREG(st.st_mode) || st.st_nlink != 1 || st.st_size < 0 ||
        static_cast<uint64_t>(st.st_size) > max)
        return fail(error, "invalid plugin file size or type");
    std::string result(static_cast<size_t>(st.st_size), '\0');
    size_t pos = 0;
    while (pos < result.size()) {
        ssize_t n = read(fd.value, &result[pos], result.size() - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail(error, "cannot read complete plugin file");
        pos += static_cast<size_t>(n);
    }
    char extra;
    ssize_t n;
    do { n = read(fd.value, &extra, 1); } while (n < 0 && errno == EINTR);
    if (n != 0) return fail(error, "plugin file changed while reading");
    data.swap(result);
    return true;
}
bool directoryNames(int fd, std::vector<std::string> &names, std::string &error) {
    // openat rather than dup: independent directory offsets on repeated scans.
    int scan = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (scan < 0) return systemFail(error, "open plugin directory scan");
    DIR *dir = fdopendir(scan);
    if (!dir) { close(scan); return systemFail(error, "scan plugin directory"); }
    bool ok = true;
    errno = 0;
    while (dirent *entry = readdir(dir)) {
        std::string name(entry->d_name);
        if (name == "." || name == "..") continue;
        if (names.size() == kMaxDirectoryEntries) {
            ok = fail(error, "plugin directory entry limit exceeded"); break;
        }
        names.push_back(name);
        errno = 0;
    }
    if (ok && errno) ok = systemFail(error, "read plugin directory");
    closedir(dir);
    std::sort(names.begin(), names.end());
    return ok;
}
bool lockRoot(int root, Fd &lock, std::string &error) {
    lock.value = openat(root, ".lock", O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (lock.value < 0) return systemFail(error, "open plugin lock");
    struct stat st{};
    if (fstat(lock.value, &st) < 0 || !S_ISREG(st.st_mode) || st.st_nlink != 1)
        return fail(error, "invalid plugin lock");
    if (flock(lock.value, LOCK_EX) < 0) return systemFail(error, "lock plugin root");
    return true;
}
bool metadataAt(int dir, const std::string &id, PackageMetadata &metadata, std::string &error) {
    std::string manifest;
    if (!readFile(dir, ".checksums", 2048, manifest, error)) return false;
    const auto *raw = reinterpret_cast<const uint8_t *>(manifest.data());
    if (manifest.size() < 16 || std::memcmp(raw, "AXMODULE", 8) || le(raw + 8, 4) != 1)
        return fail(error, "invalid installed checksum manifest");
    uint64_t count = le(raw + 12, 4);
    if (count < 2 || count > kMaxEntries) return fail(error, "invalid installed manifest count");
    size_t pos = 16, total = 16;
    std::vector<uint8_t> archive(raw, raw + 16);
    std::set<std::string> expected{".checksums"};
    for (uint64_t i = 0; i < count; ++i) {
        if (manifest.size() - pos < 20) return fail(error, "truncated installed manifest");
        const uint8_t *header = raw + pos;
        uint64_t kind = le(header, 4), nameSize = le(header + 4, 4), bytes = le(header + 8, 8);
        uint32_t checksum = static_cast<uint32_t>(le(header + 16, 4));
        pos += 20;
        if (!nameSize || nameSize > kMaxEntryName || nameSize > manifest.size() - pos)
            return fail(error, "invalid installed manifest name");
        std::string name(manifest.data() + pos, static_cast<size_t>(nameSize));
        pos += static_cast<size_t>(nameSize);
        size_t limit = 0;
        if (kind == Metadata && name == "axm.txt") limit = kMaxMetadataBytes;
        else if (kind == Module && name == "module.so") limit = kMaxModuleBytes;
        else if (kind == Asset && validAssetName(name)) limit = kMaxAssetBytes;
        else return fail(error, "invalid installed manifest entry");
        if (bytes > limit || bytes + nameSize + 20 > kMaxPackageBytes - total ||
            !expected.insert(name).second) return fail(error, "invalid installed manifest bounds");
        total += static_cast<size_t>(bytes + nameSize + 20);
        std::string content;
        if (!readFile(dir, name, limit, content, error)) return false;
        if (content.size() != bytes ||
            crc(reinterpret_cast<const uint8_t *>(content.data()), content.size()) != checksum)
            return fail(error, "installed plugin checksum mismatch");
        archive.insert(archive.end(), header, header + 20 + nameSize);
        archive.insert(archive.end(), content.begin(), content.end());
    }
    if (pos != manifest.size()) return fail(error, "trailing installed manifest data");
    std::vector<std::string> names;
    if (!directoryNames(dir, names, error)) return false;
    if (std::set<std::string>(names.begin(), names.end()) != expected)
        return fail(error, "unexpected installed plugin entries");
    Package package;
    if (!parsePackage(archive.data(), archive.size(), package, error, true)) return false;
    metadata = package.metadata;
    if (id != metadata.id) return fail(error, "installed plugin id mismatch");
    return true;
}
bool listAt(int root, std::vector<PackageMetadata> &packages, std::string &error) {
    std::vector<std::string> names;
    if (!directoryNames(root, names, error)) return false;
    for (const std::string &name : names) {
        if (name == ".lock" || name.compare(0, 7, ".stage-") == 0 ||
            name.compare(0, 8, ".remove-") == 0) continue;
        if (!validId(name)) return fail(error, "unexpected entry in plugin root");
        if (packages.size() == kMaxInstalled) return fail(error, "installed plugin limit exceeded");
        Fd dir(openat(root, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (dir.value < 0) return systemFail(error, "open installed plugin");
        PackageMetadata metadata;
        if (!metadataAt(dir.value, name, metadata, error)) return false;
        packages.push_back(metadata);
    }
    return true;
}
bool writeFile(int dir, const Entry &entry, std::string &error) {
    Fd fd(openat(dir, entry.name.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644));
    if (fd.value < 0) return systemFail(error, "create plugin file");
    size_t pos = 0;
    while (pos < entry.data.size()) {
        ssize_t n = write(fd.value, entry.data.data() + pos, entry.data.size() - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return systemFail(error, "write plugin file");
        pos += static_cast<size_t>(n);
    }
    if (fchmod(fd.value, 0644) < 0) return systemFail(error, "set plugin file mode");
    if (fsync(fd.value) < 0) return systemFail(error, "sync plugin file");
    return true;
}
bool stageDirectory(int root, const char *prefix, std::string &name, int &fd, std::string &error) {
    for (unsigned i = 0; i < 128; ++i) {
        name = std::string(prefix) + std::to_string(getpid()) + "-" + std::to_string(i);
        if (mkdirat(root, name.c_str(), 0700) == 0) {
            fd = openat(root, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (fd >= 0) return true;
            unlinkat(root, name.c_str(), AT_REMOVEDIR);
            return systemFail(error, "open staging directory");
        }
        if (errno != EEXIST) return systemFail(error, "create staging directory");
    }
    return fail(error, "too many stale plugin staging directories");
}
} // namespace

bool validId(const std::string &id) {
    return id.compare(0, 7, "plugin_") == 0 && identifier(id.substr(7), 16);
}
bool validAssetName(const std::string &name) {
    if (name.size() > kMaxEntryName) return false;
    size_t dot = name.find('.');
    if (dot == std::string::npos || !identifier(name.substr(0, dot), kMaxEntryName)) return false;
    const std::string ext = name.substr(dot);
    return ext == ".js" || ext == ".css" || ext == ".html";
}
bool parseMetadata(const std::string &text, PackageMetadata &metadata, std::string &error) {
    error.clear();
    if (text.empty() || text.size() > kMaxMetadataBytes) return fail(error, "invalid metadata size");
    for (unsigned char c : text)
        if (c != '\n' && (c < 32 || c > 126)) return fail(error, "metadata must be printable ASCII and LF");
    PackageMetadata result;
    std::set<std::string> keys, params, controls;
    size_t pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        const std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        const size_t equal = line.find('=');
        if (equal == std::string::npos) return fail(error, "metadata line requires key=value");
        std::string key = line.substr(0, equal), value = line.substr(equal + 1);
        if (key != "param" && key != "control" && key != "text" &&
            !keys.insert(key).second) return fail(error, "duplicate metadata key");
        if (key == "id") {
            if (!validId(value)) return fail(error, "invalid plugin id");
            std::memcpy(result.id, value.c_str(), value.size() + 1);
        } else if (key == "name") {
            if (!printable(value, 63)) return fail(error, "invalid plugin name");
            std::memcpy(result.name, value.c_str(), value.size() + 1);
        } else if (key == "version") {
            if (!printable(value, 31)) return fail(error, "invalid plugin version");
            std::memcpy(result.version, value.c_str(), value.size() + 1);
        } else if (key == "api") {
            if (value != "1") return fail(error, "unsupported plugin API");
        } else if (key == "param") {
            if (result.paramCount + result.textCount == AUDIOX_PLUGIN_MAX_PARAMS) return fail(error, "too many plugin controls");
            std::array<std::string, 5> fields;
            size_t start = 0;
            for (size_t i = 0; i < fields.size(); ++i) {
                size_t split = value.find('|', start);
                if ((i < 4 && split == std::string::npos) || (i == 4 && split != std::string::npos))
                    return fail(error, "parameter requires five fields");
                fields[i] = value.substr(start, split == std::string::npos ? split : split - start);
                start = split == std::string::npos ? value.size() : split + 1;
            }
            auto &param = result.params[result.paramCount];
            if (!identifier(fields[0], 23) || !params.insert(fields[0]).second ||
                !printable(fields[1], 63) || !floatValue(fields[2], param.min) ||
                !floatValue(fields[3], param.max) || !floatValue(fields[4], param.defaultValue) ||
                param.min >= param.max || param.defaultValue < param.min || param.defaultValue > param.max)
                return fail(error, "invalid parameter specification");
            std::memcpy(param.name, fields[0].c_str(), fields[0].size() + 1);
            std::memcpy(param.label, fields[1].c_str(), fields[1].size() + 1);
            ++result.paramCount;
        } else if (key == "control" || key == "text") {
            const size_t count = key == "control" ? 6 : 4;
            std::vector<std::string> fields;
            size_t start = 0;
            for (;;) {
                const size_t split = value.find('|', start);
                fields.push_back(value.substr(start, split == std::string::npos ? split : split - start));
                if (split == std::string::npos) break;
                start = split + 1;
            }
            if (fields.size() != count) return fail(error, "invalid control field count");
            if (!identifier(fields[0], 23)) return fail(error, "invalid control name");
            if (key == "text") {
                if (result.paramCount + result.textCount == AUDIOX_PLUGIN_MAX_PARAMS ||
                    !params.insert(fields[0]).second || !printable(fields[1], 63) ||
                    fields[2].size() > 255 || fields[3].size() > 255)
                    return fail(error, "invalid text control");
                auto& textControl = result.texts[result.textCount++];
                std::strcpy(textControl.name, fields[0].c_str());
                std::strcpy(textControl.label, fields[1].c_str());
                std::strcpy(textControl.defaultValue, fields[2].c_str());
                std::strcpy(textControl.description, fields[3].c_str());
                continue;
            }
            ParameterSpec* param = nullptr;
            for (uint32_t i = 0; i < result.paramCount; ++i)
                if (fields[0] == result.params[i].name) param = &result.params[i];
            if (!param || !controls.insert(fields[0]).second ||
                (fields[1] != "float" && fields[1] != "integer" &&
                 fields[1] != "boolean" && fields[1] != "enum") ||
                fields[2].size() > 23 || fields[4].size() > 255 || fields[5].size() > 255 ||
                !floatValue(fields[3], param->step) || param->step <= 0)
                return fail(error, "invalid numeric control");
            const bool discrete = fields[1] != "float";
            if (discrete && (param->step != 1 || std::floor(param->min) != param->min ||
                             std::floor(param->max) != param->max ||
                             std::floor(param->defaultValue) != param->defaultValue ||
                             std::fabs(param->min) > 16777216 || std::fabs(param->max) > 16777216))
                return fail(error, "discrete controls require exact integer bounds, defaults and step 1");
            if (fields[1] == "boolean" && (param->min != 0 || param->max != 1))
                return fail(error, "boolean controls require bounds 0 and 1");
            if (fields[1] == "enum") {
                const size_t choices = 1 + std::count(fields[5].begin(), fields[5].end(), '~');
                if (fields[5].empty() || fields[5].front() == '~' || fields[5].back() == '~' ||
                    fields[5].find("~~") != std::string::npos || param->min != 0 ||
                    param->max != choices - 1 || choices > 32)
                    return fail(error, "enum choices must match zero-based bounds");
            } else if (!fields[5].empty()) return fail(error, "choices require enum type");
            std::strcpy(param->type, fields[1].c_str());
            std::strcpy(param->unit, fields[2].c_str());
            std::strcpy(param->description, fields[4].c_str());
            std::strcpy(param->options, fields[5].c_str());
        } else return fail(error, "unknown metadata key");
    }
    if (keys != std::set<std::string>{"id", "name", "version", "api"})
        return fail(error, "missing required metadata key");
    metadata = result;
    return true;
}

bool parsePackage(const uint8_t *data, size_t size, Package &package, std::string &error,
                  bool requireNative) {
    error.clear();
    if (!data || size < 16 || size > kMaxPackageBytes) return fail(error, "invalid package size");
    if (std::memcmp(data, "AXMODULE", 8)) return fail(error, "invalid package magic");
    if (le(data + 8, 4) != 1) return fail(error, "unsupported package format version");
    uint64_t count = le(data + 12, 4);
    if (count < 2 || count > kMaxEntries) return fail(error, "invalid package entry count");
    Package result;
    std::set<std::string> names;
    bool hasMetadata = false, hasModule = false;
    size_t pos = 16;
    for (uint64_t i = 0; i < count; ++i) {
        if (size - pos < 20) return fail(error, "truncated package entry");
        Entry entry;
        entry.kind = static_cast<uint32_t>(le(data + pos, 4));
        uint64_t nameLength = le(data + pos + 4, 4), bytes = le(data + pos + 8, 8);
        uint32_t checksum = static_cast<uint32_t>(le(data + pos + 16, 4));
        pos += 20;
        if (!nameLength || nameLength > kMaxEntryName || nameLength > size - pos)
            return fail(error, "invalid package entry name length");
        entry.name.assign(reinterpret_cast<const char *>(data + pos), static_cast<size_t>(nameLength));
        pos += static_cast<size_t>(nameLength);
        if (!names.insert(entry.name).second) return fail(error, "duplicate package entry");
        size_t limit = 0;
        if (entry.kind == Metadata && entry.name == "axm.txt" && !hasMetadata) {
            limit = kMaxMetadataBytes; hasMetadata = true;
        } else if (entry.kind == Module && entry.name == "module.so" && !hasModule) {
            limit = kMaxModuleBytes; hasModule = true;
        } else if (entry.kind == Asset && validAssetName(entry.name)) limit = kMaxAssetBytes;
        else return fail(error, "invalid package entry kind or name");
        if (bytes > limit || bytes > size - pos) return fail(error, "invalid package entry size");
        if (crc(data + pos, static_cast<size_t>(bytes)) != checksum)
            return fail(error, "package checksum mismatch");
        entry.data.assign(data + pos, data + pos + bytes);
        pos += static_cast<size_t>(bytes);
        if (entry.kind == Metadata &&
            !parseMetadata(std::string(entry.data.begin(), entry.data.end()), result.metadata, error)) return false;
        if (entry.kind == Module && !elf(entry.data, requireNative, error)) return false;
        result.entries.push_back(std::move(entry));
    }
    if (!hasMetadata || !hasModule) return fail(error, "package requires metadata and module");
    if (pos != size) return fail(error, "trailing package data");
    package = std::move(result);
    return true;
}
bool buildPackage(const std::vector<Entry> &entries, std::vector<uint8_t> &data,
                  std::string &error) {
    error.clear();
    if (entries.size() < 2 || entries.size() > kMaxEntries) return fail(error, "invalid package entry count");
    size_t size = 16;
    for (const auto &entry : entries) {
        if (entry.name.size() > kMaxEntryName || entry.data.size() > kMaxModuleBytes ||
            entry.data.size() + entry.name.size() + 20 > kMaxPackageBytes - size)
            return fail(error, "package exceeds size limits");
        size += 20 + entry.name.size() + entry.data.size();
    }
    std::vector<uint8_t> result;
    result.reserve(size);
    for (char c : std::string("AXMODULE")) result.push_back(static_cast<uint8_t>(c));
    put(result, 1, 4);
    put(result, entries.size(), 4);
    for (const auto &entry : entries) {
        put(result, entry.kind, 4); put(result, entry.name.size(), 4);
        put(result, entry.data.size(), 8); put(result, crc(entry.data.data(), entry.data.size()), 4);
        result.insert(result.end(), entry.name.begin(), entry.name.end());
        result.insert(result.end(), entry.data.begin(), entry.data.end());
    }
    Package checked;
    if (!parsePackage(result.data(), result.size(), checked, error, false)) return false;
    data.swap(result);
    return true;
}

bool installPackage(const uint8_t *data, size_t size, const std::string &root, std::string &error) {
    Package package;
    if (!parsePackage(data, size, package, error, true)) return false;
    Fd dir(openRoot(root, true, error)), lock;
    if (dir.value < 0 || !lockRoot(dir.value, lock, error)) return false;
    std::vector<PackageMetadata> installed;
    if (!listAt(dir.value, installed, error)) return false;
    if (installed.size() >= kMaxInstalled) return fail(error, "maximum installed plugins reached");
    struct stat existing{};
    if (fstatat(dir.value, package.metadata.id, &existing, AT_SYMLINK_NOFOLLOW) == 0)
        return fail(error, "plugin already exists; delete it explicitly first");
    if (errno != ENOENT) return systemFail(error, "check existing plugin");
    std::string stageName;
    Fd stage;
    if (!stageDirectory(dir.value, ".stage-", stageName, stage.value, error)) return false;
    bool ok = true;
    for (const auto &entry : package.entries) if (!writeFile(stage.value, entry, error)) { ok = false; break; }
    Entry manifest{0, ".checksums", {}};
    manifest.data.insert(manifest.data.end(), data, data + 16);
    for (const auto &entry : package.entries) {
        put(manifest.data, entry.kind, 4); put(manifest.data, entry.name.size(), 4);
        put(manifest.data, entry.data.size(), 8);
        put(manifest.data, crc(entry.data.data(), entry.data.size()), 4);
        manifest.data.insert(manifest.data.end(), entry.name.begin(), entry.name.end());
    }
    if (ok) ok = writeFile(stage.value, manifest, error);
    if (ok && fchmod(stage.value, 0755) < 0) ok = systemFail(error, "set plugin directory mode");
    if (ok && fsync(stage.value) < 0) ok = systemFail(error, "sync staged plugin");
    if (ok && syscall(SYS_renameat2, dir.value, stageName.c_str(), dir.value,
                      package.metadata.id, 1 /* RENAME_NOREPLACE */) < 0)
        ok = systemFail(error, "install staged plugin");
    if (!ok) {
        for (const auto &entry : package.entries) unlinkat(stage.value, entry.name.c_str(), 0);
        unlinkat(stage.value, ".checksums", 0);
        unlinkat(dir.value, stageName.c_str(), AT_REMOVEDIR);
        fsync(dir.value);
        return false;
    }
    if (fsync(dir.value) < 0) return systemFail(error, "sync installed plugin root");
    return true;
}
bool listPackages(const std::string &root, std::vector<PackageMetadata> &packages, std::string &error) {
    error.clear();
    Fd dir(openRoot(root, false, error));
    if (dir.value < 0) {
        if (errno == ENOENT) { error.clear(); packages.clear(); return true; }
        return false;
    }
    Fd lock;
    if (!lockRoot(dir.value, lock, error)) return false;
    std::vector<PackageMetadata> result;
    if (!listAt(dir.value, result, error)) return false;
    packages.swap(result);
    return true;
}
bool removePackage(const std::string &id, const std::string &root, std::string &error) {
    error.clear();
    if (!validId(id)) return fail(error, "invalid plugin id");
    Fd dir(openRoot(root, false, error));
    if (dir.value < 0) return false;
    Fd lock;
    if (!lockRoot(dir.value, lock, error)) return false;
    Fd package(openat(dir.value, id.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (package.value < 0) return systemFail(error, "open plugin to remove");
    std::vector<std::string> names;
    if (!directoryNames(package.value, names, error)) return false;
    if (names.size() > kMaxEntries + 1) return fail(error, "invalid installed package entry count");
    for (const auto &name : names) {
        if (name != "axm.txt" && name != "module.so" && name != ".checksums" && !validAssetName(name))
            return fail(error, "unexpected file in plugin directory");
        struct stat st{};
        if (fstatat(package.value, name.c_str(), &st, AT_SYMLINK_NOFOLLOW) < 0)
            return systemFail(error, "stat plugin removal entry");
        if (!S_ISREG(st.st_mode)) return fail(error, "plugin removal refuses non-regular files");
    }
    for (const auto &name : names)
        if (unlinkat(package.value, name.c_str(), 0) < 0) return systemFail(error, "remove plugin file");
    if (fsync(package.value) < 0) return systemFail(error, "sync removed plugin files");
    if (unlinkat(dir.value, id.c_str(), AT_REMOVEDIR) < 0) return systemFail(error, "remove plugin directory");
    if (fsync(dir.value) < 0) return systemFail(error, "sync removed plugin root");
    return true;
}
bool readPluginAsset(const std::string &id, const std::string &name,
                     const std::string &root, std::string &data, std::string &error) {
    error.clear();
    if (!validId(id) || !validAssetName(name)) return fail(error, "invalid plugin asset path");
    Fd dir(openRoot(root, false, error));
    if (dir.value < 0) return false;
    Fd lock;
    if (!lockRoot(dir.value, lock, error)) return false;
    Fd package(openat(dir.value, id.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
    if (package.value < 0) return systemFail(error, "open plugin asset directory");
    PackageMetadata metadata;
    if (!metadataAt(package.value, id, metadata, error)) return false;
    return readFile(package.value, name, kMaxAssetBytes, data, error);
}
} // namespace audiox::plugin
