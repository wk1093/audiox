#include "hardware_modules.hpp"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fnmatch.h>
#include <linux/netlink.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

constexpr int kMaxColdplugPasses = 8;
constexpr int kMaxWalkDepth = 40;
constexpr int kMaxDependencyDepth = 16;

int finitLoader(const char *path, void *) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -errno;
    int rc = static_cast<int>(syscall(SYS_finit_module, fd, "", 0));
    int error = rc < 0 ? errno : 0;
    close(fd);
    return rc < 0 ? -error : 0;
}

bool readSmallFile(const std::string &path, std::string &out) {
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return false;
    char buffer[4096];
    ssize_t count;
    do {
        count = read(fd, buffer, sizeof(buffer) - 1);
    } while (count < 0 && errno == EINTR);
    close(fd);
    if (count <= 0) return false;
    while (count > 0 && (buffer[count - 1] == '\n' || buffer[count - 1] == '\r' || buffer[count - 1] == ' ')) --count;
    out.assign(buffer, static_cast<size_t>(count));
    return !out.empty();
}

std::string aliasBucket(const std::string &text) {
    size_t colon = text.find(':');
    if (colon == std::string::npos) return std::string();
    std::string prefix = text.substr(0, colon);
    if (prefix.find_first_of("*?[") != std::string::npos) return std::string();
    return prefix;
}

std::vector<std::string> splitWords(const char *text) {
    std::vector<std::string> words;
    const char *p = text;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
        const char *start = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') ++p;
        if (p > start) words.emplace_back(start, static_cast<size_t>(p - start));
    }
    return words;
}

}  // namespace

std::string hardwareModuleName(const std::string &nameOrPath) {
    size_t slash = nameOrPath.rfind('/');
    std::string name = slash == std::string::npos ? nameOrPath : nameOrPath.substr(slash + 1);
    for (const char *suffix : {".xz", ".zst", ".gz", ".ko"}) {
        size_t len = strlen(suffix);
        if (name.size() > len && name.compare(name.size() - len, len, suffix) == 0) name.resize(name.size() - len);
    }
    for (char &c : name) {
        if (c == '-') c = '_';
    }
    return name;
}

HardwareModules::HardwareModules() : sysRoot_("/sys"), loader_(finitLoader) {}

HardwareModules::~HardwareModules() {
    if (ueventFd_ >= 0) close(ueventFd_);
}

void HardwareModules::setLoader(Loader loader, void *user) {
    loader_ = loader ? loader : finitLoader;
    loaderUser_ = user;
}

void HardwareModules::configure(const char *modulesDir, const char *sysRoot) {
    modulesDir_ = modulesDir ? modulesDir : "";
    sysRoot_ = sysRoot ? sysRoot : "/sys";
    aliases_.clear();
    paths_.clear();
    deps_.clear();
    softdeps_.clear();
    seenModaliases_.clear();
    aliasTotal_ = 0;
    loadMetadata();
}

void HardwareModules::loadMetadata() {
    char line[1024];
    FILE *fp = fopen((modulesDir_ + "/modules.dep").c_str(), "re");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            if (line[0] == '#') continue;
            char *colon = strchr(line, ':');
            if (!colon) continue;
            *colon = '\0';
            const std::string name = hardwareModuleName(line);
            paths_[name] = modulesDir_ + "/" + line;
            std::vector<std::string> &deps = deps_[name];
            for (const std::string &dep : splitWords(colon + 1)) deps.push_back(hardwareModuleName(dep));
        }
        fclose(fp);
    }

    fp = fopen((modulesDir_ + "/modules.alias").c_str(), "re");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            std::vector<std::string> words = splitWords(line);
            if (words.size() != 3 || words[0] != "alias") continue;
            Alias alias{words[1], hardwareModuleName(words[2])};
            if (!paths_.count(alias.module)) continue;
            aliases_[aliasBucket(alias.pattern)].push_back(alias);
            ++aliasTotal_;
        }
        fclose(fp);
    }

    fp = fopen((modulesDir_ + "/modules.softdep").c_str(), "re");
    if (fp) {
        while (fgets(line, sizeof(line), fp)) {
            std::vector<std::string> words = splitWords(line);
            if (words.size() < 3 || words[0] != "softdep") continue;
            SoftDep &softdep = softdeps_[hardwareModuleName(words[1])];
            std::vector<std::string> *target = nullptr;
            for (size_t i = 2; i < words.size(); ++i) {
                if (words[i] == "pre:") target = &softdep.pre;
                else if (words[i] == "post:") target = &softdep.post;
                else if (target) target->push_back(hardwareModuleName(words[i]));
            }
        }
        fclose(fp);
    }
}

bool HardwareModules::openUevents() {
    if (ueventFd_ >= 0) return true;
    int fd = socket(AF_NETLINK, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (fd < 0) {
        printf("[MODULES] [WARN] kernel uevent socket unavailable: %s; hotplugged hardware needs a reboot\n", strerror(errno));
        return false;
    }
    // Coldplug and display bring-up can emit hundreds of events at once.
    int size = 1 << 20;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVBUFFORCE, &size, sizeof(size)) < 0) {
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    }
    sockaddr_nl address{};
    address.nl_family = AF_NETLINK;
    address.nl_groups = 1;  // kernel uevent multicast group
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) < 0) {
        printf("[MODULES] [WARN] cannot bind kernel uevent socket: %s; hotplugged hardware needs a reboot\n", strerror(errno));
        close(fd);
        return false;
    }
    ueventFd_ = fd;
    return true;
}

bool HardwareModules::matchModalias(const std::string &modalias, std::vector<std::string> &modules) const {
    auto check = [&](const std::vector<Alias> &bucket) {
        for (const Alias &alias : bucket) {
            if (fnmatch(alias.pattern.c_str(), modalias.c_str(), 0) != 0) continue;
            bool duplicate = false;
            for (const std::string &module : modules) duplicate = duplicate || module == alias.module;
            if (!duplicate) modules.push_back(alias.module);
        }
    };
    auto it = aliases_.find(aliasBucket(modalias));
    if (it != aliases_.end() && !it->first.empty()) check(it->second);
    auto wildcard = aliases_.find(std::string());
    if (wildcard != aliases_.end()) check(wildcard->second);
    return !modules.empty();
}

bool HardwareModules::isLoaded(const std::string &name) const {
    if (loaded_.count(name)) return true;
    struct stat st{};
    return stat((sysRoot_ + "/module/" + name).c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool HardwareModules::loadByName(const std::string &name, const char *reason) {
    return loadInternal(hardwareModuleName(name), reason, 0);
}

bool HardwareModules::loadInternal(const std::string &name, const char *reason, int depth) {
    if (isLoaded(name)) {
        loaded_.insert(name);
        return true;
    }
    if (attempted_.count(name) || depth > kMaxDependencyDepth) return false;
    auto path = paths_.find(name);
    if (path == paths_.end()) return false;  // not staged (built in, or not wanted)
    attempted_.insert(name);

    auto softdep = softdeps_.find(name);
    if (softdep != softdeps_.end()) {
        for (const std::string &pre : softdep->second.pre) loadInternal(pre, name.c_str(), depth + 1);
    }
    auto deps = deps_.find(name);
    if (deps != deps_.end()) {
        for (const std::string &dep : deps->second) {
            if (!loadInternal(dep, name.c_str(), depth + 1) && !isLoaded(dep)) {
                printf("[MODULES] [WARN] %s: dependency %s is not loaded\n", name.c_str(), dep.c_str());
            }
        }
    }

    int rc = loader_(path->second.c_str(), loaderUser_);
    if (rc < 0 && rc != -EEXIST) {
        printf("[MODULES] [ERR] loading %s (%s) failed: %s\n", name.c_str(), reason, strerror(-rc));
        return false;
    }
    loaded_.insert(name);
    printf("[MODULES] loaded %s (%s)\n", name.c_str(), reason);
    if (softdep != softdeps_.end()) {
        for (const std::string &post : softdep->second.post) loadInternal(post, name.c_str(), depth + 1);
    }
    return true;
}

int HardwareModules::handleModalias(const std::string &modalias, const char *device) {
    if (modalias.empty() || !seenModaliases_.insert(modalias).second) return 0;
    std::vector<std::string> modules;
    if (!matchModalias(modalias, modules)) return 0;
    int loaded = 0;
    for (const std::string &module : modules) {
        if (isLoaded(module) || attempted_.count(module)) continue;
        if (loadInternal(module, device, 0)) ++loaded;
    }
    return loaded;
}

int HardwareModules::walk(const std::string &dir, int depth) {
    if (depth > kMaxWalkDepth) return 0;
    int loaded = 0;
    struct stat st{};
    // Devices that already have a driver need nothing.
    if (lstat((dir + "/driver").c_str(), &st) != 0) {
        std::string modalias;
        if (readSmallFile(dir + "/modalias", modalias)) {
            loaded += handleModalias(modalias, dir.c_str() + sysRoot_.size());
        }
    }
    DIR *handle = opendir(dir.c_str());
    if (!handle) return loaded;
    std::vector<std::string> children;
    while (dirent *entry = readdir(handle)) {
        if (entry->d_name[0] == '.') continue;
        if (entry->d_type == DT_DIR) {
            children.emplace_back(entry->d_name);
        } else if (entry->d_type == DT_UNKNOWN) {
            std::string child = dir + "/" + entry->d_name;
            if (lstat(child.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) children.emplace_back(entry->d_name);
        }
    }
    closedir(handle);
    for (const std::string &child : children) loaded += walk(dir + "/" + child, depth + 1);
    return loaded;
}

int HardwareModules::coldplug() {
    if (paths_.empty() || aliasTotal_ == 0) return 0;
    int total = 0;
    for (int pass = 0; pass < kMaxColdplugPasses; ++pass) {
        // Newly loaded drivers create child devices (I2C muxes, DSI hosts...),
        // so walk again until nothing new loads. Seen modaliases are skipped.
        int loaded = walk(sysRoot_ + "/devices", 0);
        total += loaded;
        if (loaded == 0) break;
    }
    return total;
}

void HardwareModules::handleUevent(const char *message, size_t length, PollResult &result) {
    const char *action = nullptr;
    const char *subsystem = nullptr;
    const char *modalias = nullptr;
    const char *devpath = nullptr;
    size_t offset = 0;
    while (offset < length) {
        const char *field = message + offset;
        size_t fieldLength = strnlen(field, length - offset);
        if (strncmp(field, "ACTION=", 7) == 0) action = field + 7;
        else if (strncmp(field, "SUBSYSTEM=", 10) == 0) subsystem = field + 10;
        else if (strncmp(field, "MODALIAS=", 9) == 0) modalias = field + 9;
        else if (strncmp(field, "DEVPATH=", 8) == 0) devpath = field + 8;
        offset += fieldLength + 1;
    }
    if (!action) return;
    if (subsystem && strcmp(subsystem, "drm") == 0) result.displayChanged = true;
    if (modalias && strcmp(action, "add") == 0) {
        result.modulesLoaded += handleModalias(modalias, devpath ? devpath : modalias);
    }
}

HardwareModules::PollResult HardwareModules::poll() {
    PollResult result;
    if (ueventFd_ < 0) return result;
    char buffer[8192];
    for (int i = 0; i < 256; ++i) {
        sockaddr_nl sender{};
        socklen_t senderLength = sizeof(sender);
        ssize_t count = recvfrom(ueventFd_, buffer, sizeof(buffer) - 1, 0,
                                 reinterpret_cast<sockaddr *>(&sender), &senderLength);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == ENOBUFS) {
                // Events were dropped; rescan so no device is left without a driver.
                printf("[MODULES] [WARN] uevent overflow; rescanning devices\n");
                seenModaliases_.clear();
                result.modulesLoaded += coldplug();
                result.displayChanged = true;
                continue;
            }
            break;
        }
        if (sender.nl_pid != 0) continue;  // only trust the kernel
        buffer[count] = '\0';
        handleUevent(buffer, static_cast<size_t>(count), result);
    }
    return result;
}
