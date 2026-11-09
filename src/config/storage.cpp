#include "config/storage.hpp"
#include "config/schema.hpp"
#include "watchdog_heartbeat.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace audiox::config_storage {
namespace {
struct Error {
    char *text;
    size_t size;
    int fail(const char *operation) {
        snprintf(text, size, "%s: %s", operation, strerror(errno));
        return -1;
    }
};

int syncDirectory(const std::string &path) {
    int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
    if (fd < 0) return -1;
    int rc = fsync(fd), saved = errno;
    if (close(fd) < 0 && rc == 0) return -1;
    errno = saved;
    return rc;
}

int removeTree(const std::string &path) {
    struct stat st{};
    if (lstat(path.c_str(), &st) < 0) return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode)) return unlink(path.c_str());
    DIR *dir = opendir(path.c_str());
    if (!dir) return -1;
    int rc = 0;
    for (;;) {
        errno = 0;
        dirent *entry = readdir(dir);
        if (!entry) { if (errno) rc = -1; break; }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        audioxWatchdogHeartbeat();
        if (removeTree(path + "/" + entry->d_name) < 0) { rc = -1; break; }
    }
    int saved = errno;
    if (closedir(dir) < 0 && rc == 0) return -1;
    errno = saved;
    return rc < 0 ? rc : rmdir(path.c_str());
}

int copyTree(const std::string &src, const std::string &dst, unsigned depth = 0) {
    struct stat st{};
    if (lstat(src.c_str(), &st) < 0) return errno == ENOENT && depth == 0 ? 0 : -1;
    if (depth > 16 || (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode))) {
        errno = EINVAL;
        return -1;
    }
    audioxWatchdogHeartbeat();
    if (S_ISDIR(st.st_mode)) {
        if (mkdir(dst.c_str(), 0700) < 0) return -1;
        DIR *dir = opendir(src.c_str());
        if (!dir) return -1;
        int rc = 0;
        for (;;) {
            errno = 0;
            dirent *entry = readdir(dir);
            if (!entry) { if (errno) rc = -1; break; }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (copyTree(src + "/" + entry->d_name, dst + "/" + entry->d_name, depth + 1) < 0) {
                rc = -1;
                break;
            }
        }
        int saved = errno;
        if (closedir(dir) < 0 && rc == 0) return -1;
        errno = saved;
        return rc < 0 ? rc : syncDirectory(dst);
    }
    int in = open(src.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
    if (in < 0) return -1;
    int out = open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (out < 0) { int saved = errno; close(in); errno = saved; return -1; }
    int rc = 0;
    char buffer[65536];
    for (;;) {
        audioxWatchdogHeartbeat();
        ssize_t n = read(in, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { rc = -1; break; }
        if (!n) break;
        ssize_t pos = 0;
        while (pos < n) {
            ssize_t written = write(out, buffer + pos, static_cast<size_t>(n - pos));
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) { if (!written) errno = EIO; rc = -1; break; }
            pos += written;
        }
        if (rc < 0) break;
    }
    if (rc == 0) rc = fsync(out);
    int saved = errno;
    if (close(in) < 0 && rc == 0) { rc = -1; saved = errno; }
    if (close(out) < 0 && rc == 0) { rc = -1; saved = errno; }
    errno = saved;
    return rc;
}

bool scriptLine(const std::string &line) {
    size_t start = line.find_first_not_of(" \t");
    size_t end = line.find('=');
    if (start == std::string::npos || end == std::string::npos || start >= end) return false;
    std::string key = line.substr(start, end - start);
    key.erase(key.find_last_not_of(" \t") + 1);
    return key == "automation" || key == "automation_events" ||
           key == "automation_config_write" || key == "automation_hooks" ||
           key == "key_chord" || key == "key_macro";
}

int atomicText(const std::string &path, const std::string &text) {
    std::string temp = path + ".migration";
    FILE *file = fopen(temp.c_str(), "w");
    if (!file) return -1;
    bool ok = fwrite(text.data(), 1, text.size(), file) == text.size();
    if (ok) ok = fflush(file) == 0 && fsync(fileno(file)) == 0;
    int saved = errno;
    if (fclose(file) != 0 && ok) { ok = false; saved = errno; }
    if (!ok) { errno = saved; return -1; }
    if (rename(temp.c_str(), path.c_str()) < 0) return -1;
    return syncDirectory(path.substr(0, path.find_last_of('/')));
}

int layoutVersion(const std::string &config, Error &error) {
    struct stat st{};
    if (lstat(config.c_str(), &st) < 0) return error.fail("inspect configuration directory");
    if (!S_ISDIR(st.st_mode)) { errno = EINVAL; return error.fail("invalid configuration directory"); }
    FILE *file = fopen((config + "/.layout-version").c_str(), "r");
    if (!file) return error.fail("read configuration layout version");
    char version[16]{};
    bool ok = fgets(version, sizeof(version), file) && !strcmp(version, "1\n") && fgetc(file) == EOF &&
              !ferror(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) { errno = EINVAL; return error.fail("unsupported configuration layout version"); }
    return 0;
}
}

int splitScripts(const char *configDir, char *text, size_t size) {
    Error error{text, size};
    const std::string dir(configDir), map = dir + "/midi_map.txt", scripts = dir + "/scripts.txt";
    const auto schema = checkConfigTextFileSchema(map.c_str(), "# audiox midi map v", MIDI_MAP_SCHEMA_VERSION);
    if (schema == ConfigTextSchemaStatus::Unsupported || schema == ConfigTextSchemaStatus::Invalid ||
        schema == ConfigTextSchemaStatus::IoError) {
        errno = EINVAL;
        return error.fail("cannot split unsupported MIDI configuration");
    }
    struct stat st{};
    bool hasScripts = lstat(scripts.c_str(), &st) == 0;
    if (!hasScripts && errno != ENOENT) return error.fail("inspect script definitions");
    if (hasScripts && !S_ISREG(st.st_mode)) { errno = EINVAL; return error.fail("invalid script definitions"); }
    if (hasScripts) {
        const auto status = checkConfigTextFileSchema(scripts.c_str(), "# audiox scripts v", SCRIPTS_SCHEMA_VERSION);
        if (status == ConfigTextSchemaStatus::Unsupported || status == ConfigTextSchemaStatus::Invalid ||
            status == ConfigTextSchemaStatus::IoError) {
            errno = EINVAL;
            return error.fail("unsupported script definitions");
        }
    }
    std::string midiText, scriptText = "# audiox scripts v1\n";
    FILE *file = fopen(map.c_str(), "r");
    bool found = false;
    if (file) {
        char *line = nullptr;
        size_t capacity = 0;
        ssize_t length;
        while ((length = getline(&line, &capacity, file)) >= 0) {
            std::string value(line, static_cast<size_t>(length));
            if (scriptLine(value)) { scriptText += value; found = true; }
            else midiText += value;
        }
        free(line);
        bool ok = !ferror(file);
        if (fclose(file) != 0) ok = false;
        if (!ok) return error.fail("read legacy MIDI scripts");
    } else if (errno != ENOENT) return error.fail("open legacy MIDI scripts");
    // Publish scripts first; a retry after power loss must never replace them.
    if (!hasScripts && atomicText(scripts, scriptText) < 0) return error.fail("save standalone scripts");
    if (found && atomicText(map, midiText) < 0) return error.fail("remove migrated script definitions from MIDI config");
    return 0;
}

int migrate(const char *root, char *text, size_t size) {
    Error error{text, size};
    std::string base(root), config = base + "/config", pending = base + "/.config-migration";
    struct stat st{};
    if (lstat(config.c_str(), &st) == 0) {
        if (!S_ISDIR(st.st_mode)) { errno = EINVAL; return error.fail("configuration is not a directory"); }
        return layoutVersion(config, error);
    }
    if (errno != ENOENT) return error.fail("inspect configuration directory");
    if (removeTree(pending) < 0 || mkdir(pending.c_str(), 0700) < 0)
        return error.fail("prepare configuration migration");
    for (const char *name : legacyNames) {
        if (copyTree(base + "/" + name, pending + "/" + name) < 0)
            return error.fail("copy legacy configuration (original data preserved)");
    }
    if (splitScripts(pending.c_str(), text, size) < 0) return -1;
    if (atomicText(pending + "/.layout-version", "1\n") < 0 ||
        syncDirectory(pending) < 0 || rename(pending.c_str(), config.c_str()) < 0 ||
        syncDirectory(base) < 0) return error.fail("publish migrated configuration");
    printf("[CONFIG] configuration migrated; legacy files retained until confirmed cleanup\n");
    return 0;
}

int legacyCount(const char *root, char *text, size_t size) {
    Error error{text, size};
    int count = 0;
    for (const char *name : legacyNames) {
        struct stat st{};
        if (lstat((std::string(root) + "/" + name).c_str(), &st) == 0) ++count;
        else if (errno != ENOENT) return error.fail("inspect legacy configuration");
    }
    return count;
}

int removeLegacy(const char *root, char *text, size_t size) {
    Error error{text, size};
    if (layoutVersion(std::string(root) + "/config", error) < 0) return -1;
    for (const char *name : legacyNames) {
        if (removeTree(std::string(root) + "/" + name) < 0) return error.fail("remove legacy configuration");
    }
    if (syncDirectory(root) < 0) return error.fail("sync legacy configuration cleanup");
    return 0;
}
}
