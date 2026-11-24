#pragma once

#include "inventory.hpp"
#include "../../include/desktop/rules.hpp"
#include "../../include/json.hpp"

#include <algorithm>
#include <sys/prctl.h>

namespace desktop {

inline bool pulseCommand(const std::vector<std::string> &arguments, int connection, std::string &output,
                         size_t maxOutput = 32768) {
    return command("pactl", arguments, connection, output, maxOutput, 750);
}

struct PulseStream {
    unsigned id{}, destination{};
    std::string serial, app, node;
};

struct PulseDevice {
    unsigned id{};
    std::string name, node, channelMap, vendor, product, mediaClass;
};

inline bool jsonNumber(std::string_view value, unsigned &number) {
    return streamNumber(value, number);
}

inline bool jsonTextMember(std::string_view object, std::string_view key, std::string &text) {
    std::string_view value;
    return jsonMember(object, key, value) && jsonString(value, text);
}

template<typename T, typename Parse>
inline bool pulseArray(std::string_view text, std::vector<T> &result, Parse parse) {
    JsonCursor cursor(text);
    if (!cursor.take('[')) return false;
    std::vector<T> parsed;
    if (!cursor.take(']')) {
        for (;;) {
            std::string_view object;
            T item;
            if (!cursor.next(object) || !parse(object, item) || parsed.size() == 512) return false;
            parsed.push_back(std::move(item));
            if (cursor.take(']')) break;
            if (!cursor.take(',')) return false;
        }
    }
    if (!cursor.finished()) return false;
    result.swap(parsed);
    return true;
}

inline bool pulseStreams(int connection, bool capture, std::vector<PulseStream> &result) {
    std::string text;
    if (!pulseCommand({"-f", "json", "list", capture ? "source-outputs" : "sink-inputs"},
                      connection, text, 1024 * 1024)) return false;
    return pulseArray(text, result, [capture](std::string_view object, PulseStream &stream) {
        std::string_view id, destination, properties;
        if (!jsonMember(object, "index", id) || !jsonNumber(id, stream.id) ||
            !jsonMember(object, capture ? "source" : "sink", destination) ||
            !jsonNumber(destination, stream.destination) || !jsonMember(object, "properties", properties))
            return false;
        (void)jsonTextMember(properties, "object.serial", stream.serial);
        (void)jsonTextMember(properties, "application.name", stream.app);
        (void)jsonTextMember(properties, "node.name", stream.node);
        return true;
    });
}

inline bool pulseDevices(int connection, bool capture, std::vector<PulseDevice> &result) {
    std::string text;
    if (!pulseCommand({"-f", "json", "list", capture ? "sources" : "sinks"},
                      connection, text, 1024 * 1024)) return false;
    return pulseArray(text, result, [](std::string_view object, PulseDevice &device) {
        std::string_view id, properties;
        if (!jsonMember(object, "index", id) || !jsonNumber(id, device.id) ||
            !jsonTextMember(object, "channel_map", device.channelMap) ||
            !jsonMember(object, "properties", properties)) return false;
        (void)jsonTextMember(properties, "node.name", device.node);
        (void)jsonTextMember(properties, "device.vendor.id", device.vendor);
        (void)jsonTextMember(properties, "device.product.id", device.product);
        (void)jsonTextMember(properties, "media.class", device.mediaClass);
        if (!jsonTextMember(object, "name", device.name)) device.name = device.node;
        if (device.name.empty()) return false;
        return true;
    });
}

inline bool pulseCookie(int connection, std::string &cookie) {
    std::string text;
    std::vector<std::string> args;
    args.emplace_back("-f");
    args.emplace_back("json");
    args.emplace_back("info");
    if (!pulseCommand(args, connection, text)) return false;
    return jsonTextMember(text, "cookie", cookie) && !cookie.empty();
}

inline bool pairMap(const std::string &map, unsigned pair, std::string &result) {
    std::vector<std::string_view> channels;
    std::string_view input(map);
    while (!input.empty()) {
        const size_t comma = input.find(',');
        const auto channel = input.substr(0, comma);
        if (channel.empty()) return false;
        for (char c : channel)
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
        channels.push_back(channel);
        if (comma == std::string_view::npos) break;
        input.remove_prefix(comma + 1);
    }
    if (channels.size() % 2 || pair >= channels.size() / 2) return false;
    result = std::string(channels[pair * 2]) + "," + std::string(channels[pair * 2 + 1]);
    return true;
}

class AutoRouting {
    struct Change {
        bool capture;
        unsigned stream, original, current, module, target, pair;
        std::string serial, app, node, moduleName;
    };
    int connection;
    uint32_t sequence{};
    size_t candidateCursor{};
    std::string cookie;
    std::vector<Rule> rules;
    std::vector<Change> changes;
    struct VirtualChannel {
        pid_t pid;
        bool capture;
        unsigned device, pair;
        std::string name;
    };
    bool virtualEnabled = true;
    std::vector<VirtualChannel> virtualChannels;
    std::string publishedPolicy;
    bool policyMissingReported = false;

    static std::string virtualName(const PulseDevice &device, bool capture, unsigned pair) {
        uint64_t hash = 14695981039346656037ull;
        for (unsigned char c : device.name) { hash ^= c; hash *= 1099511628211ull; }
        return "audiox_auto_" + std::string(capture ? "c_" : "p_") +
            std::to_string(hash) + "_" + std::to_string(pair + 1);
    }

    struct Forward {
        pid_t pid;
        unsigned source, sink, pair;
        std::string name;
    };
    std::vector<Forward> forwards;
    unsigned forwardSequence{};

    static std::string position(const std::string &value) {
        if (value == "front-left") return "FL";
        if (value == "front-right") return "FR";
        if (value == "rear-left") return "RL";
        if (value == "rear-right") return "RR";
        if (value == "side-left") return "SL";
        if (value == "side-right") return "SR";
        if (value == "front-center") return "FC";
        if (value == "lfe") return "LFE";
        if (value == "rear-center") return "RC";
        if (value == "front-left-of-center") return "FLC";
        if (value == "front-right-of-center") return "FRC";
        if (value.compare(0, 3, "aux") == 0) return "AUX" + value.substr(3);
        return std::string();
    }

    static void stopLoopback(pid_t &pid, const char *kind) {
        if (pid <= 0) return;
        if (kill(pid, SIGTERM) < 0 && errno != ESRCH)
            std::fprintf(stderr, "AudioX: could not stop %s: %s\n", kind, std::strerror(errno));
        bool exited = false;
        for (unsigned i = 0; i < 50; ++i) {
            const pid_t result = waitpid(pid, nullptr, WNOHANG);
            if (result == pid || (result < 0 && errno == ECHILD)) { exited = true; break; }
            if (result < 0 && errno != EINTR) break;
            usleep(10000);
        }
        if (!exited) {
            if (kill(pid, SIGKILL) < 0 && errno != ESRCH)
                std::fprintf(stderr, "AudioX: could not terminate %s: %s\n", kind, std::strerror(errno));
            while (waitpid(pid, nullptr, 0) < 0 && errno == EINTR) {}
        }
        pid = 0;
    }

    static void stopVirtual(VirtualChannel &channel) { stopLoopback(channel.pid, "virtual channel"); }

    pid_t spawnLoopback(const std::string &name, const std::string &capture, const std::string &playback) {
        const pid_t parent = getpid();
        const pid_t pid = fork();
        if (pid < 0) {
            std::fprintf(stderr, "AudioX: could not start loopback: %s\n", std::strerror(errno));
            return -1;
        }
        if (!pid) {
            if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0 || getppid() != parent) _exit(127);
            close(connection);
            execlp("pw-loopback", "pw-loopback", "-n", name.c_str(),
                   "-i", capture.c_str(), "-o", playback.c_str(), nullptr);
            _exit(127);
        }
        return pid;
    }

    static const PulseDevice *forwardDevice(const std::string &pattern, bool regex,
                                            const std::vector<PulseDevice> &devices) {
        const PulseDevice *found = nullptr;
        for (const auto &device : devices) {
            if (device.name.compare(0, 12, "audiox_auto_") == 0) continue;
            if (!ruleMatches(pattern, regex, device.name) &&
                (device.node.empty() || !ruleMatches(pattern, regex, device.node))) continue;
            if (found) return nullptr;
            found = &device;
        }
        return found;
    }

    // Keeps one pw-loopback per resolvable forward rule; stale or exited loopbacks are replaced.
    bool reconcileForwards(const std::vector<PulseDevice> &sinks, const std::vector<PulseDevice> &sources) {
        struct Wanted { unsigned source, sink, pair; const PulseDevice *device; };
        std::vector<Wanted> wanted;
        bool ok = true;
        for (const auto &rule : rules) {
            if (!rule.forward) continue;
            const PulseDevice *source = forwardDevice(rule.app, rule.appRegex, sources);
            const PulseDevice *sink = forwardDevice(rule.target, rule.targetRegex, sinks);
            std::string map;
            if (!source || !sink) {
                std::fprintf(stderr, "AudioX: forward source or destination unavailable or ambiguous.\n");
                ok = false;
            } else if (!pairMap(source->channelMap, rule.pair, map)) {
                std::fprintf(stderr, "AudioX: forward source %s has no stereo pair %u.\n",
                             source->name.c_str(), rule.pair + 1);
                ok = false;
            } else {
                wanted.push_back({source->id, sink->id, rule.pair, source});
            }
        }
        for (size_t i = 0; i < forwards.size();) {
            auto &forward = forwards[i];
            const pid_t result = waitpid(forward.pid, nullptr, WNOHANG);
            const bool exited = result == forward.pid || (result < 0 && errno == ECHILD);
            if (exited) {
                std::fprintf(stderr, "AudioX: forward %s stopped unexpectedly.\n", forward.name.c_str());
                forward.pid = 0;
                ok = false;
            }
            bool keep = false;
            for (const auto &item : wanted)
                if (item.source == forward.source && item.sink == forward.sink && item.pair == forward.pair)
                    keep = true;
            if (!exited && keep) { ++i; continue; }
            stopLoopback(forward.pid, "forward");
            forwards.erase(forwards.begin() + i);
        }
        for (const auto &item : wanted) {
            bool running = false;
            for (const auto &forward : forwards)
                if (item.source == forward.source && item.sink == forward.sink && item.pair == forward.pair)
                    running = true;
            if (running) continue;
            std::string map;
            (void)pairMap(item.device->channelMap, item.pair, map);
            const auto comma = map.find(',');
            const std::string left = position(map.substr(0, comma));
            const std::string right = position(map.substr(comma + 1));
            if (left.empty() || right.empty()) {
                std::fprintf(stderr, "AudioX: unsupported forward channel map %s.\n", map.c_str());
                ok = false;
                continue;
            }
            const std::string name = "audiox_auto_f_" + std::to_string(getpid()) + "_" +
                std::to_string(++forwardSequence);
            const std::string captureProps = "{ node.name=\"input." + name + "\" target.object=\"" +
                std::to_string(item.source) + "\" audio.position=[ " + left + " " + right +
                " ] stream.dont-remix=true node.dont-move=true }";
            const std::string playbackProps = "{ node.name=\"output." + name +
                "\" node.description=\"AudioX forward\" target.object=\"" + std::to_string(item.sink) +
                "\" audio.position=[ FL FR ] node.dont-move=true }";
            const pid_t pid = spawnLoopback(name, captureProps, playbackProps);
            if (pid < 0) { ok = false; continue; }
            forwards.push_back({pid, item.source, item.sink, item.pair, name});
        }
        return ok;
    }

    void stopForwards() {
        for (auto &forward : forwards) stopLoopback(forward.pid, "forward");
        forwards.clear();
    }

    bool createVirtual(const PulseDevice &device, bool capture, unsigned pair) {
        std::string map;
        if (!pairMap(device.channelMap, pair, map)) return false;
        const auto comma = map.find(',');
        const std::string left = position(map.substr(0, comma));
        const std::string right = position(map.substr(comma + 1));
        if (left.empty() || right.empty()) {
            std::fprintf(stderr, "AudioX: unsupported virtual channel map %s.\n", map.c_str());
            return false;
        }
        const std::string name = virtualName(device, capture, pair);
        const std::string description = "AudioX " + std::string(capture ? "Input " : "Channel ") +
            std::to_string(pair + 1);
        const std::string virtualProps = "{ node.name=\"" + name +
            "\" node.description=\"" + description + "\" media.class=" +
            (capture ? "Audio/Source" : "Audio/Sink") + " audio.position=[ FL FR ] }";
        const std::string physicalProps = "{ node.name=\"" + std::string(capture ? "input." : "output.") +
            name + "\" target.object=\"" + std::to_string(device.id) + "\" audio.position=[ " +
            left + " " + right + " ] stream.dont-remix=true node.dont-move=true node.passive=true }";
        const pid_t pid = spawnLoopback(name, capture ? physicalProps : virtualProps,
                                        capture ? virtualProps : physicalProps);
        if (pid < 0) return false;
        virtualChannels.push_back({pid, capture, device.id, pair, name});
        return true;
    }

    bool reconcileVirtual(const std::vector<PulseDevice> &sinks, const std::vector<PulseDevice> &sources) {
        for (size_t i = 0; i < virtualChannels.size();) {
            auto &channel = virtualChannels[i];
            const auto &devices = channel.capture ? sources : sinks;
            bool exists = false;
            for (const auto &device : devices)
                if (device.id == channel.device && device.vendor == "0x6666" && device.product == "0x0ad0")
                    exists = true;
            int status;
            const pid_t result = waitpid(channel.pid, &status, WNOHANG);
            if (result == channel.pid || (result < 0 && errno == ECHILD)) {
                std::fprintf(stderr, "AudioX: virtual channel %s stopped unexpectedly.\n", channel.name.c_str());
                channel.pid = 0;
                virtualChannels.erase(virtualChannels.begin() + i);
                return false;
            }
            if (!virtualEnabled || !exists) {
                stopVirtual(channel);
                virtualChannels.erase(virtualChannels.begin() + i);
                continue;
            }
            ++i;
        }
        if (!virtualEnabled) return true;
        for (bool capture : {false, true}) {
            const auto &devices = capture ? sources : sinks;
            for (const auto &device : devices) {
                if (device.vendor != "0x6666" || device.product != "0x0ad0" ||
                    (capture && device.mediaClass != "Audio/Source")) continue;
                for (unsigned pair = 0; pair < 16; ++pair) {
                    std::string map;
                    if (!pairMap(device.channelMap, pair, map)) break;
                    bool found = false;
                    for (const auto &channel : virtualChannels)
                        if (channel.capture == capture && channel.device == device.id && channel.pair == pair)
                            found = true;
                    if (!found && !createVirtual(device, capture, pair)) return false;
                }
            }
        }
        return true;
    }

    bool publishPolicy(const std::vector<PulseDevice> &sinks, const std::vector<PulseDevice> &sources) {
        std::string json = "[";
        bool first = true;
        bool needsPolicy = false;
        if (virtualEnabled) for (auto it = rules.rbegin(); it != rules.rend(); ++it) {
            const auto &rule = *it;
            if (rule.forward) continue;
            const PulseDevice *device = target(rule, rule.capture ? sources : sinks);
            const bool useVirtual = device && device->vendor == "0x6666" && device->product == "0x0ad0" &&
                (!rule.capture || device->mediaClass == "Audio/Source");
            needsPolicy = needsPolicy || useVirtual;
            std::string pattern;
            if (rule.appRegex) pattern = rule.app;
            else {
                pattern = "^";
                for (char c : rule.app) {
                    if (std::string_view(".^$*+?()[]{}|\\").find(c) != std::string_view::npos)
                        pattern += '\\';
                    pattern += c;
                }
                pattern += '$';
            }
            const std::string match = "~" + pattern;
            const std::string targetName = useVirtual ? virtualName(*device, rule.capture, rule.pair) : "";
            const auto &devices = rule.capture ? sources : sinks;
            bool ready = !useVirtual;
            for (const auto &candidate : devices) if (candidate.name == targetName) ready = true;
            if (!ready) {
                std::fprintf(stderr, "AudioX: virtual destination %s is not ready.\n", targetName.c_str());
                return false;
            }
            const std::string kind = rule.capture ? "Stream/Input/Audio" : "Stream/Output/Audio";
            if (!first) json += ',';
            first = false;
            json += "{\"matches\":[{\"media.class\":\"" + kind + "\",\"application.name\":\"" +
                audiox::json::escape(match.c_str()) + "\"},{\"media.class\":\"" + kind +
                "\",\"node.name\":\"" + audiox::json::escape(match.c_str()) +
                "\"}],\"actions\":{\"update-props\":{\"audiox.target\":\"" +
                audiox::json::escape(targetName.c_str()) + "\"}}}";
        }
        json += ']';
        if (!needsPolicy) {
            json = "[]";
            policyMissingReported = false;
        }
        std::string metadata;
        if (needsPolicy) {
            if (!command("pw-metadata", {"-n", "default"}, connection, metadata, 1048576, 750) ||
                metadata.find("key:'audiox.policy' value:'1'") == std::string::npos) {
                if (!policyMissingReported)
                    std::fprintf(stderr, "AudioX: WirePlumber routing policy is not installed or active.\n");
                policyMissingReported = true;
                return false;
            }
            if (policyMissingReported)
                std::fprintf(stderr, "AudioX: WirePlumber routing policy is active.\n");
            policyMissingReported = false;
        }
        if (json == "[]" && publishedPolicy.empty()) return true;
        if (json == publishedPolicy && (!needsPolicy ||
            metadata.find("key:'audiox.routing' value:'" + json + "'") != std::string::npos)) return true;
        std::string output;
        if (!command("pw-metadata", {"-n", "default", "0", "audiox.routing", json, "Spa:String:JSON"},
                     connection, output, 32768, 750)) {
            std::fprintf(stderr, "AudioX: could not publish routing policy: %s\n", output.c_str());
            return false;
        }
        publishedPolicy = std::move(json);
        return true;
    }

    static const Rule *match(const std::vector<Rule> &rules, bool capture, const PulseStream &stream) {
        for (const auto &rule : rules) {
            if (rule.forward || rule.capture != capture) continue;
            if ((!stream.app.empty() && ruleMatches(rule.app, rule.appRegex, stream.app)) ||
                (!stream.node.empty() && ruleMatches(rule.app, rule.appRegex, stream.node))) return &rule;
        }
        return nullptr;
    }

    static const PulseDevice *target(const Rule &rule, const std::vector<PulseDevice> &devices) {
        const PulseDevice *found = nullptr;
        for (const auto &device : devices) {
            if (device.name.compare(0, 12, "audiox_auto_") == 0) continue;
            if (!ruleMatches(rule.target, rule.targetRegex, device.name) &&
                (device.node.empty() || !ruleMatches(rule.target, rule.targetRegex, device.node))) continue;
            if (found) return nullptr;
            found = &device;
        }
        return found;
    }

    bool unload(unsigned module) {
        std::string output;
        if (pulseCommand({"unload-module", std::to_string(module)}, connection, output)) return true;
        std::fprintf(stderr, "AudioX: could not remove automatic route module %u: %s\n", module, output.c_str());
        return false;
    }

    bool remove(const Change &change, const std::vector<PulseStream> &streams) {
        const PulseStream *stream = nullptr;
        bool others = false;
        for (const auto &candidate : streams) {
            if (candidate.id == change.stream && candidate.serial == change.serial) stream = &candidate;
            else if (candidate.destination == change.current) others = true;
        }
        if (stream && stream->destination == change.current) {
            std::string output;
            if (!pulseCommand({change.capture ? "move-source-output" : "move-sink-input",
                        std::to_string(change.stream), std::to_string(change.original)}, connection, output)) {
                std::fprintf(stderr, "AudioX: could not restore automatic route %u: %s\n",
                             change.stream, output.c_str());
                return false;
            }
        }
        if (others) {
            std::fprintf(stderr, "AudioX: keeping route module %u used by another stream.\n", change.module);
            return true;
        }
        return !change.module || unload(change.module);
    }

    bool create(bool capture, const PulseStream &stream, const PulseDevice &device, const Rule &rule,
                const std::vector<PulseDevice> &devices) {
        if (stream.serial.empty() || stream.destination == UINT32_MAX) {
            std::fprintf(stderr, "AudioX: cannot safely restore automatic stream %u.\n", stream.id);
            return false;
        }
        std::string channels;
        if (!pairMap(device.channelMap, rule.pair, channels)) {
            std::fprintf(stderr, "AudioX: destination %s has no stereo pair %u.\n",
                         device.name.c_str(), rule.pair + 1);
            return false;
        }
        if (virtualEnabled && device.vendor == "0x6666" && device.product == "0x0ad0" &&
            (!capture || device.mediaClass == "Audio/Source")) {
            const std::string name = virtualName(device, capture, rule.pair);
            const PulseDevice *virtualDevice = nullptr;
            for (const auto &candidate : devices) if (candidate.name == name) virtualDevice = &candidate;
            if (!virtualDevice) return false;
            if (stream.destination == virtualDevice->id) return true;
            std::string output;
            if (!pulseCommand({capture ? "move-source-output" : "move-sink-input",
                               std::to_string(stream.id), name}, connection, output)) {
                std::fprintf(stderr, "AudioX: could not move automatic stream %u: %s\n",
                             stream.id, output.c_str());
                return false;
            }
            changes.push_back({capture, stream.id, stream.destination, virtualDevice->id, 0,
                               device.id, rule.pair, stream.serial, stream.app, stream.node, name});
            return true;
        }
        const std::string name = "audiox_auto_" + std::to_string(getpid()) + "_" + std::to_string(++sequence);
        for (const auto &candidate : devices) if (candidate.name == name) return false;
        std::string output;
        if (!pulseCommand({"load-module", capture ? "module-remap-source" : "module-remap-sink",
                    std::string(capture ? "source_name=" : "sink_name=") + name,
                    "master=" + std::to_string(device.id), "channels=2",
                    "master_channel_map=" + channels, "channel_map=front-left,front-right", "remix=no"},
                   connection, output)) {
            std::fprintf(stderr, "AudioX: could not create stereo route: %s\n", output.c_str());
            return false;
        }
        while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
        unsigned module;
        if (!streamNumber(output, module)) {
            std::fprintf(stderr, "AudioX: invalid automatic route module ID: %s\n", output.c_str());
            return false;
        }
        std::vector<PulseDevice> updated;
        if (!pulseDevices(connection, capture, updated)) { (void)unload(module); return false; }
        const PulseDevice *virtualDevice = nullptr;
        for (const auto &candidate : updated) if (candidate.name == name) virtualDevice = &candidate;
        if (!virtualDevice) { (void)unload(module); return false; }
        std::string verifiedCookie;
        std::vector<PulseStream> verified;
        if (!pulseCookie(connection, verifiedCookie) || verifiedCookie != cookie ||
            !pulseStreams(connection, capture, verified)) {
            std::fprintf(stderr, "AudioX: audio server changed while preparing a route.\n");
            if (verifiedCookie == cookie) (void)unload(module);
            return false;
        }
        bool unchanged = false;
        for (const auto &candidate : verified)
            if (candidate.id == stream.id && candidate.serial == stream.serial &&
                candidate.destination == stream.destination) unchanged = true;
        if (!unchanged) {
            std::fprintf(stderr, "AudioX: stream changed while preparing a route.\n");
            (void)unload(module);
            return false;
        }
        if (!pulseCommand({capture ? "move-source-output" : "move-sink-input",
                    std::to_string(stream.id), name}, connection, output)) {
            std::fprintf(stderr, "AudioX: could not move automatic stream %u: %s\n", stream.id, output.c_str());
            (void)unload(module);
            return false;
        }
        changes.push_back({capture, stream.id, stream.destination, virtualDevice->id, module,
                           device.id, rule.pair, stream.serial, stream.app, stream.node, name});
        return true;
    }

public:
    explicit AutoRouting(int fd) : connection(fd) {}
    ~AutoRouting() { restore(); }
    AutoRouting(const AutoRouting &) = delete;
    AutoRouting &operator=(const AutoRouting &) = delete;

    bool setRules(std::vector<Rule> next, bool enabled = true) {
        if (!publishedPolicy.empty() && publishedPolicy != "[]") {
            std::string output;
            if (!command("pw-metadata", {"-n", "default", "0", "audiox.routing", "[]", "Spa:String:JSON"},
                         connection, output, 32768, 750)) {
                std::fprintf(stderr, "AudioX: could not clear old routing policy: %s\n", output.c_str());
                return false;
            }
            publishedPolicy = "[]";
        }
        rules.swap(next);
        virtualEnabled = enabled;
        return true;
    }

    bool manages(bool capture, unsigned streamId) const {
        for (const auto &change : changes)
            if (change.capture == capture && change.stream == streamId) return true;
        return false;
    }

    bool tick(const Routing *manual = nullptr) {
        if (rules.empty() && changes.empty() && !virtualEnabled && virtualChannels.empty() &&
            forwards.empty()) return true;
        std::string currentCookie;
        if (!pulseCookie(connection, currentCookie)) {
            std::fprintf(stderr, "AudioX: could not inspect audio server for automatic routing.\n");
            return false;
        }
        if (!cookie.empty() && cookie != currentCookie) {
            std::fprintf(stderr, "AudioX: audio server changed; previous routes cannot be restored.\n");
            changes.clear();
            for (auto &channel : virtualChannels) stopVirtual(channel);
            virtualChannels.clear();
            stopForwards();
            publishedPolicy.clear();
        }
        cookie = currentCookie;
        std::vector<PulseStream> playback, capture;
        std::vector<PulseDevice> sinks, sources;
        if (!pulseStreams(connection, false, playback) || !pulseStreams(connection, true, capture) ||
            !pulseDevices(connection, false, sinks) || !pulseDevices(connection, true, sources)) {
            std::fprintf(stderr, "AudioX: could not inspect audio streams for automatic routing.\n");
            return false;
        }
        if (!reconcileVirtual(sinks, sources)) return false;
        const bool forwardsOk = reconcileForwards(sinks, sources);
        bool movedExisting = false;
        for (bool isCapture : {false, true}) {
            const auto &streams = isCapture ? capture : playback;
            const auto &devices = isCapture ? sources : sinks;
            for (const auto &stream : streams) {
                if (!isCapture && manual && manual->manages(stream.id)) continue;
                if (stream.node.compare(0, 19, "output.audiox_auto_") == 0 ||
                    stream.node.compare(0, 18, "input.audiox_auto_") == 0) continue;
                if (manages(isCapture, stream.id)) continue;
                const Rule *rule = match(rules, isCapture, stream);
                const PulseDevice *device = rule ? target(*rule, devices) : nullptr;
                if (!virtualEnabled || !device || device->vendor != "0x6666" ||
                    device->product != "0x0ad0" || (isCapture && device->mediaClass != "Audio/Source"))
                    continue;
                const std::string name = virtualName(*device, isCapture, rule->pair);
                const PulseDevice *virtualDevice = nullptr;
                for (const auto &candidate : devices) if (candidate.name == name) virtualDevice = &candidate;
                if (!virtualDevice) return false;
                if (stream.destination == virtualDevice->id) continue;
                if (changes.size() >= 32 || !create(isCapture, stream, *device, *rule, devices)) return false;
                movedExisting = true;
            }
        }
        if (movedExisting) return false;
        if (!publishPolicy(sinks, sources)) return false;
        for (size_t i = 0; i < changes.size(); ++i) {
            const auto &change = changes[i];
            const auto &streams = change.capture ? capture : playback;
            const auto &devices = change.capture ? sources : sinks;
            const PulseStream *found = nullptr;
            for (const auto &stream : streams)
                if (stream.id == change.stream && stream.serial == change.serial) found = &stream;
            const Rule *rule = found ? match(rules, change.capture, *found) : nullptr;
            const PulseDevice *device = rule ? target(*rule, devices) : nullptr;
            if (!found || found->destination != change.current || !rule || !device ||
                device->id != change.target || rule->pair != change.pair) {
                const bool removed = remove(change, streams);
                if (removed) changes.erase(changes.begin() + i);
                return removed && forwardsOk;
            }
        }
        struct Candidate {
            bool capture;
            const PulseStream *stream;
            const PulseDevice *device;
            const Rule *rule;
            const std::vector<PulseDevice> *devices;
        };
        std::vector<Candidate> candidates;
        bool missing = false;
        for (bool isCapture : {false, true}) {
            const auto &streams = isCapture ? capture : playback;
            const auto &devices = isCapture ? sources : sinks;
            for (const auto &stream : streams) {
                if (!isCapture && manual && manual->manages(stream.id)) continue;
                if (stream.node.compare(0, 19, "output.audiox_auto_") == 0 ||
                    stream.node.compare(0, 18, "input.audiox_auto_") == 0) continue;
                bool already = false;
                for (const auto &change : changes)
                    if (change.capture == isCapture && change.stream == stream.id &&
                        change.serial == stream.serial) already = true;
                if (already) continue;
                const Rule *rule = match(rules, isCapture, stream);
                const PulseDevice *device = rule ? target(*rule, devices) : nullptr;
                if (rule && !device) {
                    std::fprintf(stderr, "AudioX: automatic routing destination unavailable or ambiguous.\n");
                    missing = true;
                }
                if (device) candidates.push_back({isCapture, &stream, device, rule, &devices});
            }
        }
        if (candidates.empty()) return !missing && forwardsOk;
        if (changes.size() >= 32) {
            std::fprintf(stderr, "AudioX: automatic route limit reached.\n");
            return false;
        }
        const auto &selected = candidates[candidateCursor++ % candidates.size()];
        return create(selected.capture, *selected.stream, *selected.device, *selected.rule, *selected.devices) &&
            !missing && forwardsOk;
    }

    void restore() {
        if (!publishedPolicy.empty()) {
            std::string output;
            if (!command("pw-metadata", {"-n", "default", "-d", "0", "audiox.routing"},
                         connection, output, 32768, 750))
                std::fprintf(stderr, "AudioX: could not clear routing policy: %s\n", output.c_str());
            publishedPolicy.clear();
        }
        stopForwards();
        if (changes.empty()) {
            for (auto &channel : virtualChannels) stopVirtual(channel);
            virtualChannels.clear();
            return;
        }
        std::string currentCookie;
        if (!pulseCookie(connection, currentCookie) || currentCookie != cookie) {
            std::fprintf(stderr, "AudioX: cannot safely restore automatic routes after audio server change.\n");
            for (auto &channel : virtualChannels) stopVirtual(channel);
            virtualChannels.clear();
            return;
        }
        std::vector<PulseStream> playback, capture;
        if (!pulseStreams(connection, false, playback) || !pulseStreams(connection, true, capture)) {
            std::fprintf(stderr, "AudioX: could not inspect automatic routes for restoration.\n");
            for (auto &channel : virtualChannels) stopVirtual(channel);
            virtualChannels.clear();
            return;
        }
        for (const auto &change : changes) (void)remove(change, change.capture ? capture : playback);
        changes.clear();
        for (auto &channel : virtualChannels) stopVirtual(channel);
        virtualChannels.clear();
    }
};

} // namespace desktop
