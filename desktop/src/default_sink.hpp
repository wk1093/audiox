#pragma once

#include "inventory.hpp"

namespace desktop {

class DefaultSinkControl {
    enum class Backend { Unknown, Pulse, WirePlumber };
    int connection;
    Backend backend = Backend::Unknown;

    static std::string commandError(const char *command, const std::string &output) {
        std::string message = std::string(command) + " failed";
        if (!output.empty()) {
            std::string text = output.substr(0, 300);
            while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
            if (!text.empty()) message += ": " + text;
        }
        return message;
    }

    bool pulseDefault(std::string &name, std::string &error) const {
        std::string output;
        if (!pactl({"get-default-sink"}, connection, output)) {
            error = commandError("pactl get-default-sink", output);
            return false;
        }
        if (!output.empty() && output.back() == '\n') output.pop_back();
        if (!validSinkName(output)) {
            error = "pactl returned an invalid or unavailable default sink";
            return false;
        }
        name = output;
        return true;
    }

    bool wirePlumberNode(const std::string &id, std::string &name, std::string &error) const {
        std::string output;
        if (!command("wpctl", {"inspect", id}, connection, output)) {
            error = commandError("wpctl inspect", output);
            return false;
        }
        for (size_t start = 0; start < output.size();) {
            const size_t end = output.find('\n', start);
            std::string_view line(output.data() + start,
                                  (end == std::string::npos ? output.size() : end) - start);
            while (!line.empty() && (line.front() == ' ' || line.front() == '\t' || line.front() == '*'))
                line.remove_prefix(1);
            if (consume(line, "node.name")) {
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
                if (!consume(line, "=")) break;
                while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
                while (!line.empty() && (line.back() == ' ' || line.back() == '\t' || line.back() == '\r'))
                    line.remove_suffix(1);
                std::string value;
                if (jsonString(line, value) && validSinkName(value)) {
                    name = std::move(value);
                    return true;
                }
                break;
            }
            if (end == std::string::npos) break;
            start = end + 1;
        }
        error = "wpctl did not report a valid sink node.name";
        return false;
    }

    bool pulseSet(const std::string &name, std::string &error) const {
        std::string output;
        if (!pactl({"list", "short", "sinks"}, connection, output)) {
            error = commandError("pactl list short sinks", output);
            return false;
        }
        bool found = false;
        for (size_t start = 0; start < output.size();) {
            const size_t end = output.find('\n', start);
            const size_t lineEnd = end == std::string::npos ? output.size() : end;
            const size_t first = output.find('\t', start);
            const size_t second = first == std::string::npos ? first : output.find('\t', first + 1);
            if (first < lineEnd && second <= lineEnd &&
                std::string_view(output).substr(first + 1, second - first - 1) == name) found = true;
            start = end == std::string::npos ? output.size() : end + 1;
        }
        if (!found) {
            error = "default sink '" + name + "' is unavailable";
            return false;
        }
        if (!pactl({"set-default-sink", name}, connection, output)) {
            error = commandError("pactl set-default-sink", output);
            return false;
        }
        return true;
    }

    bool wirePlumberSet(const std::string &name, std::string &error) const {
        std::string nodes;
        if (!discoverNodes(connection, nodes)) {
            error = "cannot discover PipeWire sinks with pw-dump";
            return false;
        }
        JsonCursor cursor(nodes);
        if (!cursor.take('[')) {
            error = "invalid PipeWire sink inventory";
            return false;
        }
        unsigned matchedId = 0, matches = 0;
        if (!cursor.take(']')) {
            for (;;) {
                std::string_view node, id, encodedName, encodedClass;
                std::string nodeName, mediaClass;
                unsigned nodeId;
                if (!cursor.next(node)) {
                    error = "invalid PipeWire sink inventory";
                    return false;
                }
                if (jsonMember(node, "id", id) && streamNumber(id, nodeId) && nodeId &&
                    jsonMember(node, "node.name", encodedName) && jsonString(encodedName, nodeName) &&
                    jsonMember(node, "media.class", encodedClass) && jsonString(encodedClass, mediaClass) &&
                    nodeName == name &&
                    (mediaClass == "Audio/Sink" || mediaClass.compare(0, 11, "Audio/Sink/") == 0)) {
                    matchedId = nodeId;
                    ++matches;
                }
                if (cursor.take(']')) break;
                if (!cursor.take(',')) {
                    error = "invalid PipeWire sink inventory";
                    return false;
                }
            }
        }
        if (!cursor.finished() || matches != 1) {
            error = matches ? "multiple PipeWire sinks have the requested name" :
                              "default sink '" + name + "' is unavailable";
            return false;
        }
        std::string verified;
        const std::string nodeId = std::to_string(matchedId);
        if (!wirePlumberNode(nodeId, verified, error)) return false;
        if (verified != name) {
            error = "PipeWire sink changed while resolving its node ID";
            return false;
        }
        std::string output;
        if (!command("wpctl", {"set-default", nodeId}, connection, output)) {
            error = commandError("wpctl set-default", output);
            return false;
        }
        return true;
    }

public:
    explicit DefaultSinkControl(int fd) : connection(fd) {}

    bool read(std::string &name, std::string &error) {
        error.clear();
        if (backend == Backend::Pulse) return pulseDefault(name, error);
        if (backend == Backend::WirePlumber) return wirePlumberNode("@DEFAULT_AUDIO_SINK@", name, error);
        std::string pulseError;
        if (pulseDefault(name, pulseError)) {
            backend = Backend::Pulse;
            std::fprintf(stderr, "AudioX: using pactl for desktop default sink control.\n");
            return true;
        }
        if (wirePlumberNode("@DEFAULT_AUDIO_SINK@", name, error)) {
            backend = Backend::WirePlumber;
            std::fprintf(stderr, "AudioX: using WirePlumber for desktop default sink control.\n");
            return true;
        }
        error = pulseError + "; " + error;
        return false;
    }

    bool set(const std::string &name, std::string &error) const {
        error.clear();
        if (!validSinkName(name)) {
            error = "invalid default sink name";
            return false;
        }
        if (backend == Backend::Pulse) return pulseSet(name, error);
        if (backend == Backend::WirePlumber) return wirePlumberSet(name, error);
        error = "default sink backend is unavailable";
        return false;
    }
};

} // namespace desktop
