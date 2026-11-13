#pragma once

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// Loads kernel modules for the hardware that is present, like udev/mdev does:
// each device's sysfs `modalias` is matched against the staged modules.alias
// (resolve_modules.sh writes it for the "auto:" seeds), and the matching module
// is loaded with its modules.dep dependencies and modules.softdep pre/post
// modules. Coldplug walks /sys/devices; afterwards kernel uevents (netlink)
// report new devices and display (DRM) hotplug.
class HardwareModules {
public:
    // Returns 0 on success or a negative errno. Tests replace the real
    // finit_module() loader to observe and simulate module loading.
    using Loader = int (*)(const char *path, void *user);

    struct PollResult {
        int modulesLoaded = 0;
        bool displayChanged = false;  // a DRM device appeared/changed or reported HOTPLUG=1
    };

    HardwareModules();
    ~HardwareModules();
    HardwareModules(const HardwareModules &) = delete;
    HardwareModules &operator=(const HardwareModules &) = delete;

    // modulesDir: /lib/modules/<release>; sysRoot: /sys. Missing metadata is not
    // an error (no auto modules staged); the loader then does nothing.
    void configure(const char *modulesDir, const char *sysRoot = "/sys");
    void setLoader(Loader loader, void *user);

    // Opens the kernel uevent socket. Call before coldplug() so that no device
    // added during the walk is missed.
    bool openUevents();
    int ueventFd() const { return ueventFd_; }

    // Walks sysfs devices until a pass loads nothing new; returns modules loaded.
    int coldplug();
    // Drains pending uevents without blocking.
    PollResult poll();

    // Loads one module (by name) plus dependencies; true if it is now loaded.
    bool loadByName(const std::string &name, const char *reason);
    // Number of aliases indexed (for logging and tests).
    size_t aliasCount() const { return aliasTotal_; }

    // Handles one raw uevent message (NUL-separated). Exposed for tests.
    void handleUevent(const char *message, size_t length, PollResult &result);

private:
    struct Alias {
        std::string pattern;
        std::string module;
    };
    struct SoftDep {
        std::vector<std::string> pre;
        std::vector<std::string> post;
    };

    void loadMetadata();
    bool matchModalias(const std::string &modalias, std::vector<std::string> &modules) const;
    int handleModalias(const std::string &modalias, const char *device);
    bool isLoaded(const std::string &name) const;
    bool loadInternal(const std::string &name, const char *reason, int depth);
    int walk(const std::string &dir, int depth);

    std::string modulesDir_;
    std::string sysRoot_;
    Loader loader_;
    void *loaderUser_ = nullptr;
    int ueventFd_ = -1;
    size_t aliasTotal_ = 0;
    // Aliases bucketed by their bus prefix ("of", "i2c", "usb", ...); patterns
    // whose prefix contains a wildcard live under "".
    std::unordered_map<std::string, std::vector<Alias>> aliases_;
    std::unordered_map<std::string, std::string> paths_;
    std::unordered_map<std::string, std::vector<std::string>> deps_;
    std::unordered_map<std::string, SoftDep> softdeps_;
    std::unordered_set<std::string> attempted_;
    std::unordered_set<std::string> loaded_;
    std::unordered_set<std::string> seenModaliases_;
};

std::string hardwareModuleName(const std::string &nameOrPath);
