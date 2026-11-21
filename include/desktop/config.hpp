#pragma once

#include "config/file.hpp"
#include "config/schema.hpp"
#include "desktop/rules.hpp"
#include "defs.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace desktop {

inline constexpr const char *rulesPath = CONFIG_ROOT_DIR "/desktop-routes.txt";

inline bool loadRules(std::string &text) {
    const int fd = open(rulesPath, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { text = rulesHeader; return true; }
        std::fprintf(stderr, "[DESKTOP] [WARN] Cannot open routing rules: %s\n", std::strerror(errno));
        return false;
    }
    struct stat status{};
    bool ok = fstat(fd, &status) == 0 && S_ISREG(status.st_mode) && status.st_size >= 0 &&
        status.st_size <= 16384;
    std::string input;
    char buffer[1024];
    while (ok) {
        const ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { ok = false; break; }
        if (!count) break;
        input.append(buffer, static_cast<size_t>(count));
        if (input.size() > 16384) ok = false;
    }
    if (close(fd) < 0) ok = false;
    std::vector<Rule> rules;
    if (!ok || !parseRules(input, rules)) {
        std::fprintf(stderr, "[DESKTOP] [WARN] Invalid or unreadable routing rules.\n");
        return false;
    }
    text = std::move(input);
    return true;
}

inline bool saveRules(const std::string &text) {
    std::vector<Rule> parsed;
    if (!parseRules(text, parsed)) return false;
    std::string existing;
    if (!loadRules(existing)) return false;
    audiox::AtomicConfigFile file(rulesPath);
    if (!file.get() || fwrite(text.data(), 1, text.size(), file.get()) != text.size() || file.commit() < 0) {
        std::fprintf(stderr, "[DESKTOP] [WARN] Cannot save routing rules: %s\n", std::strerror(errno));
        return false;
    }
    return true;
}

} // namespace desktop
