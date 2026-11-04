#include "scripting/storage.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
constexpr size_t SETTINGS_MAX = 16384;
int fail(int error) { errno = error; return RET_ERR; }
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) { const int saved = errno; close(value); errno = saved; } }
};
bool regular(const struct stat &st, size_t maximum) {
    return S_ISREG(st.st_mode) && st.st_nlink == 1 && st.st_size >= 0 &&
        static_cast<uint64_t>(st.st_size) <= maximum;
}
int scan(int fd, std::vector<std::string> &names, size_t &total, bool cleanup = false) {
    names.clear();
    total = 0;
    // dup shares the directory offset; open "." for an independent enumeration.
    int copy = openat(fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (copy < 0) return RET_ERR;
    DIR *dir = fdopendir(copy);
    if (!dir) { close(copy); return RET_ERR; }
    int result = RET_OK;
    unsigned pendingCount = 0;
    bool removed = false;
    errno = 0;
    while (dirent *entry = readdir(dir)) {
        const char *name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) continue;
        bool settings = !strcmp(name, SCRIPT_STORAGE_SETTINGS_NAME);
        bool pending = !strncmp(name, ".write-", 7) && ScriptStorage::validName(name + 1);
        struct stat st{};
        if ((!settings && !pending && !ScriptStorage::validName(name)) ||
            fstatat(fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0 ||
            !regular(st, settings || pending ? SETTINGS_MAX : SCRIPT_STORAGE_FILE_MAX)) {
            result = fail(EINVAL); break;
        }
        if (pending) {
            if (++pendingCount > SCRIPT_ACTIONS_MAX) { result = fail(EDQUOT); break; }
            if (cleanup) {
                if (unlinkat(fd, name, 0) < 0) { result = RET_ERR; break; }
                removed = true;
                errno = 0;
                continue;
            }
        }
        total += static_cast<size_t>(st.st_size);
        if (!settings && !pending) names.emplace_back(name);
        if (names.size() > SCRIPT_STORAGE_FILES_MAX || total > SCRIPT_STORAGE_TOTAL_MAX) {
            result = fail(EDQUOT); break;
        }
        errno = 0;
    }
    if (result == RET_OK && errno) result = RET_ERR;
    if (result == RET_OK && removed && fsync(fd) < 0) result = RET_ERR;
    const int saved = errno;
    closedir(dir);
    errno = saved;
    std::sort(names.begin(), names.end());
    return result;
}
std::string hex(const char *text) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(text); *p; ++p) {
        result += digits[*p >> 4]; result += digits[*p & 15];
    }
    return result;
}
bool unhex(const std::string &text, char *out, size_t capacity) {
    if (text.size() % 2 || text.size() / 2 >= capacity) return false;
    auto digit = [](char c) { return c >= '0' && c <= '9' ? c - '0' :
        c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; };
    for (size_t i = 0; i < text.size(); i += 2) {
        int high = digit(text[i]), low = digit(text[i + 1]);
        if (high < 0 || low < 0 || !(high || low)) return false;
        out[i / 2] = static_cast<char>(high * 16 + low);
    }
    out[text.size() / 2] = 0;
    return true;
}
}

ScriptStorage::ScriptStorage(uint32_t id, const char *root) : id_(id), root_(root ? root : "") {}

bool ScriptStorage::validName(const char *name) {
    if (!name) return false;
    size_t length = strnlen(name, SCRIPT_STORAGE_NAME_MAX + 1);
    if (!length || length > SCRIPT_STORAGE_NAME_MAX || name[0] == '.' ||
        strstr(name, "..")) return false;
    for (size_t i = 0; i < length; ++i) {
        unsigned char c = static_cast<unsigned char>(name[i]);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

bool ScriptStorage::validSetting(const ScriptStateEntry &entry) {
    size_t length = strnlen(entry.key, sizeof(entry.key));
    if (!length || length == sizeof(entry.key)) return false;
    if (entry.kind == ScriptValueKind::Boolean) return true;
    if (entry.kind == ScriptValueKind::String)
        return strnlen(entry.stringValue, sizeof(entry.stringValue)) < sizeof(entry.stringValue);
    return entry.kind == ScriptValueKind::Number && std::isfinite(entry.numberValue) &&
        (!entry.numberIsInteger || entry.numberValue == static_cast<double>(entry.integerValue));
}

int ScriptStorage::openRoot(bool create) const {
    if (root_.empty()) return fail(EINVAL);
    Fd current(open(root_[0] == '/' ? "/" : ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (current.value < 0) return RET_ERR;
    size_t start = root_[0] == '/' ? 1 : 0;
    while (start < root_.size()) {
        size_t end = root_.find('/', start);
        if (end == std::string::npos) end = root_.size();
        std::string part = root_.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") return fail(EINVAL);
        if (create) {
            if (mkdirat(current.value, part.c_str(), 0700) == 0) {
                if (fsync(current.value) < 0) return RET_ERR;
            } else if (errno != EEXIST) return RET_ERR;
        }
        int next = openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0) return RET_ERR;
        close(current.value); current.value = next;
    }
    int result = current.value; current.value = -1; return result;
}

int ScriptStorage::openDirectory(bool create) const {
    if (!id_) return fail(EINVAL);
    Fd root(openRoot(create));
    if (root.value < 0) return RET_ERR;
    char name[16]; snprintf(name, sizeof(name), "%u", id_);
    if (create) {
        if (mkdirat(root.value, name, 0700) == 0) {
            if (fsync(root.value) < 0) return RET_ERR;
        } else if (errno != EEXIST) return RET_ERR;
    }
    return openat(root.value, name, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
}

int ScriptStorage::read(const char *name, std::string &text) const {
    text.clear();
    Fd directory(openDirectory(false));
    if (directory.value < 0) return RET_ERR;
    Fd file(openat(directory.value, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    if (file.value < 0) return RET_ERR;
    struct stat st{};
    size_t max = !strcmp(name, SCRIPT_STORAGE_SETTINGS_NAME) ? SETTINGS_MAX : SCRIPT_STORAGE_FILE_MAX;
    if (fstat(file.value, &st) < 0) return RET_ERR;
    if (!regular(st, max)) return fail(EINVAL);
    char buffer[1024];
    for (;;) {
        ssize_t size = ::read(file.value, buffer, sizeof(buffer));
        if (size < 0) { if (errno == EINTR) continue; text.clear(); return RET_ERR; }
        if (!size) break;
        if (static_cast<size_t>(size) > max - text.size()) { text.clear(); return fail(EFBIG); }
        text.append(buffer, static_cast<size_t>(size));
    }
    if (!strcmp(name, SCRIPT_STORAGE_SETTINGS_NAME) && text.find('\0') != std::string::npos) {
        text.clear(); return fail(EINVAL);
    }
    return RET_OK;
}
int ScriptStorage::readFile(const char *name, std::string &text) const {
    text.clear();
    if (!validName(name)) return fail(EINVAL);
    return read(name, text);
}
int ScriptStorage::listFiles(std::vector<std::string> &names) const {
    names.clear();
    Fd directory(openDirectory(false));
    if (directory.value < 0) return errno == ENOENT ? RET_OK : RET_ERR;
    if (flock(directory.value, LOCK_SH) < 0) return RET_ERR;
    size_t total = 0;
    return scan(directory.value, names, total);
}

int ScriptStorage::write(const char *name, const char *text, size_t length, bool settings) const {
    if (!text || length > (settings ? SETTINGS_MAX : SCRIPT_STORAGE_FILE_MAX) ||
        (settings && memchr(text, '\0', length))) return fail(EINVAL);
    Fd directory(openDirectory(true));
    if (directory.value < 0 || flock(directory.value, LOCK_EX) < 0) return RET_ERR;
    return writeLocked(directory.value, name, text, length, settings);
}
int ScriptStorage::writeLocked(int directory, const char *name, const char *text, size_t length, bool settings) const {
    std::vector<std::string> names;
    size_t total = 0;
    if (scan(directory, names, total, true) != RET_OK) return RET_ERR;
    struct stat old{};
    bool exists = fstatat(directory, name, &old, AT_SYMLINK_NOFOLLOW) == 0;
    if (!exists && errno != ENOENT) return RET_ERR;
    if (exists && !regular(old, settings ? SETTINGS_MAX : SCRIPT_STORAGE_FILE_MAX)) return fail(EINVAL);
    if ((!settings && !exists && names.size() >= SCRIPT_STORAGE_FILES_MAX) ||
        total - (exists ? static_cast<size_t>(old.st_size) : 0) + length > SCRIPT_STORAGE_TOTAL_MAX)
        return fail(EDQUOT);
    char pending[64];
    unsigned attempt = 0;
    int fd = -1;
    for (; attempt < 128; ++attempt) {
        snprintf(pending, sizeof(pending), ".write-%ld-%u", static_cast<long>(getpid()), attempt);
        fd = openat(directory, pending, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) return RET_ERR;
    Fd file(fd);
    int result = RET_OK;
    size_t written = 0;
    while (written < length) {
        ssize_t size = ::write(fd, text + written, length - written);
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) { if (!size) errno = EIO; result = RET_ERR; break; }
        written += static_cast<size_t>(size);
    }
    if (result == RET_OK && fsync(fd) < 0) result = RET_ERR;
    if (close(file.value) < 0 && result == RET_OK) result = RET_ERR;
    file.value = -1;
    if (result == RET_OK && renameat(directory, pending, directory, name) < 0) result = RET_ERR;
    int saved = errno;
    if (result != RET_OK) unlinkat(directory, pending, 0);
    else if (fsync(directory) < 0) { saved = errno; result = RET_ERR; }
    errno = saved;
    return result;
}
int ScriptStorage::writeFile(const char *name, const char *text, size_t length) const {
    if (!validName(name)) return fail(EINVAL);
    return write(name, text, length, false);
}
int ScriptStorage::removeFile(const char *name) const {
    if (!validName(name)) return fail(EINVAL);
    Fd directory(openDirectory(false));
    if (directory.value < 0) return errno == ENOENT ? RET_OK : RET_ERR;
    if (flock(directory.value, LOCK_EX) < 0) return RET_ERR;
    struct stat st{};
    if (fstatat(directory.value, name, &st, AT_SYMLINK_NOFOLLOW) < 0)
        return errno == ENOENT ? RET_OK : RET_ERR;
    if (!regular(st, SCRIPT_STORAGE_FILE_MAX)) return fail(EINVAL);
    if (unlinkat(directory.value, name, 0) < 0 || fsync(directory.value) < 0) return RET_ERR;
    return RET_OK;
}

int ScriptStorage::readSettings(ScriptState &settings) const {
    settings = {};
    std::string text;
    if (read(SCRIPT_STORAGE_SETTINGS_NAME, text) != RET_OK)
        return errno == ENOENT ? RET_OK : RET_ERR;
    if (text.compare(0, 4, "AX1\n")) return fail(EINVAL);
    size_t start = 4;
    while (start < text.size()) {
        size_t end = text.find('\n', start), tab = text.find('\t', start);
        if (end == std::string::npos || tab == std::string::npos || tab + 2 >= end ||
            text[tab + 2] != '\t' || settings.count == SCRIPT_STATE_ENTRIES_MAX) return fail(EINVAL);
        ScriptStateEntry entry{};
        if (!unhex(text.substr(start, tab - start), entry.key, sizeof(entry.key))) return fail(EINVAL);
        std::string value = text.substr(tab + 3, end - tab - 3);
        char *tail = nullptr;
        errno = 0;
        switch (text[tab + 1]) {
        case 'b':
            if (value != "0" && value != "1") return fail(EINVAL);
            entry.kind = ScriptValueKind::Boolean; entry.booleanValue = value == "1"; break;
        case 'i':
            entry.kind = ScriptValueKind::Number; entry.numberIsInteger = true;
            entry.integerValue = strtoll(value.c_str(), &tail, 10);
            entry.numberValue = static_cast<double>(entry.integerValue);
            if (errno || value.empty() || !tail || *tail) return fail(EINVAL);
            break;
        case 'n':
            entry.kind = ScriptValueKind::Number;
            entry.numberValue = strtod(value.c_str(), &tail);
            if (errno || value.empty() || !tail || *tail) return fail(EINVAL);
            break;
        case 's':
            entry.kind = ScriptValueKind::String;
            if (!unhex(value, entry.stringValue, sizeof(entry.stringValue))) return fail(EINVAL);
            break;
        default: return fail(EINVAL);
        }
        if (!validSetting(entry)) return fail(EINVAL);
        for (unsigned i = 0; i < settings.count; ++i)
            if (!strcmp(settings.entries[i].key, entry.key)) return fail(EINVAL);
        settings.entries[settings.count++] = entry;
        start = end + 1;
    }
    return RET_OK;
}
int ScriptStorage::setSetting(const ScriptStateEntry &entry, bool remove) const {
    if (remove ? (!entry.key[0] || strnlen(entry.key, sizeof(entry.key)) == sizeof(entry.key)) :
        !validSetting(entry)) return fail(EINVAL);
    Fd directory(openDirectory(true));
    if (directory.value < 0 || flock(directory.value, LOCK_EX) < 0) return RET_ERR;
    ScriptState state{};
    if (readSettings(state) != RET_OK) return RET_ERR;
    unsigned index = 0;
    while (index < state.count && strcmp(state.entries[index].key, entry.key)) ++index;
    if (remove) {
        if (index < state.count) state.entries[index] = state.entries[--state.count];
    } else {
        if (index == state.count) {
            if (state.count == SCRIPT_STATE_ENTRIES_MAX) return fail(EDQUOT);
            ++state.count;
        }
        state.entries[index] = entry;
    }
    std::string text = "AX1\n";
    for (unsigned i = 0; i < state.count; ++i) {
        const auto &value = state.entries[i];
        text += hex(value.key); text += '\t';
        char number[64];
        if (value.kind == ScriptValueKind::String) text += "s\t" + hex(value.stringValue);
        else if (value.kind == ScriptValueKind::Boolean) text += value.booleanValue ? "b\t1" : "b\t0";
        else {
            if (value.numberIsInteger) snprintf(number, sizeof(number), "i\t%lld", static_cast<long long>(value.integerValue));
            else snprintf(number, sizeof(number), "n\t%.17g", value.numberValue);
            text += number;
        }
        text += '\n';
    }
    return writeLocked(directory.value, SCRIPT_STORAGE_SETTINGS_NAME, text.data(), text.size(), true);
}

int ScriptStorage::nextId(uint32_t minimum, uint32_t &out) const {
    out = 0;
    if (!minimum) minimum = 1;
    Fd root(openRoot(false));
    if (root.value < 0) {
        if (errno != ENOENT) return RET_ERR;
        out = minimum; return RET_OK;
    }
    int copy = openat(root.value, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (copy < 0) return RET_ERR;
    DIR *dir = fdopendir(copy);
    if (!dir) { close(copy); return RET_ERR; }
    uint64_t candidate = minimum;
    unsigned count = 0;
    int result = RET_OK;
    errno = 0;
    while (dirent *entry = readdir(dir)) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++count > 65536) { result = fail(EFBIG); break; }
        const char *name = entry->d_name;
        bool numeric = *name != '\0';
        for (const char *p = name; *p; ++p) if (*p < '0' || *p > '9') numeric = false;
        if (numeric) {
            errno = 0;
            unsigned long long value = strtoull(name, nullptr, 10);
            if (errno || value > UINT32_MAX) { result = fail(EOVERFLOW); break; }
            if (value >= candidate) candidate = value + 1;
        }
        errno = 0;
    }
    if (result == RET_OK && errno) result = RET_ERR;
    int saved = errno; closedir(dir); errno = saved;
    if (result != RET_OK) return result;
    if (candidate > UINT32_MAX) return fail(EOVERFLOW);
    out = static_cast<uint32_t>(candidate);
    return RET_OK;
}
