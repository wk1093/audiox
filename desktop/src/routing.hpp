#pragma once

#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

namespace desktop {

inline bool pactl(const std::vector<std::string> &arguments, int connection, std::string &output) {
    int pipefd[2];
    if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) != 0) return false;
    const pid_t child = fork();
    if (child < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (child == 0) {
        close(connection);
        close(pipefd[0]);
        const int flags = fcntl(pipefd[1], F_GETFL);
        if (flags < 0 || fcntl(pipefd[1], F_SETFL, flags & ~O_NONBLOCK) < 0 ||
            dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0)
            _exit(127);
        close(pipefd[1]);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>("pactl"));
        for (const auto &argument : arguments) argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        execvp("pactl", argv.data());
        _exit(127);
    }
    close(pipefd[1]);
    output.clear();
    bool eof = false, exited = false, failed = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!failed && !(eof && exited)) {
        char buffer[512];
        for (;;) {
            const ssize_t count = read(pipefd[0], buffer, sizeof(buffer));
            if (count > 0) {
                output.append(buffer, static_cast<size_t>(count));
                if (output.size() > 32768) { failed = true; break; }
            } else if (count == 0) { eof = true; break; }
            else if (errno == EINTR) continue;
            else if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            else { failed = true; break; }
        }
        if (!exited) {
            const pid_t result = waitpid(child, &status, WNOHANG);
            if (result == child) exited = true;
            else if (result < 0 && errno != EINTR) failed = true;
        }
        if (failed || (eof && exited)) break;
        if (std::chrono::steady_clock::now() >= deadline) { failed = true; break; }
        pollfd fd{pipefd[0], POLLIN, 0};
        if (poll(&fd, 1, 50) < 0 && errno != EINTR) failed = true;
    }
    close(pipefd[0]);
    if (!exited) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    return !failed && exited && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

struct Stream {
    unsigned id;
    unsigned sink;
    unsigned client;
};

inline bool streams(int connection, std::vector<Stream> &result) {
    std::string output;
    if (!pactl({"list", "short", "sink-inputs"}, connection, output)) return false;
    result.clear();
    size_t pos = 0;
    while (pos < output.size()) {
        const size_t end = output.find('\n', pos);
        const std::string line = output.substr(pos, end == std::string::npos ? end : end - pos);
        unsigned id, sink, client;
        int consumed = 0;
        if (std::sscanf(line.c_str(), "%u\t%u\t%u\t%n", &id, &sink, &client, &consumed) != 3 ||
            !consumed) return false;
        if (result.size() == 256) return false;
        result.push_back({id, sink, client});
        if (end == std::string::npos) break;
        pos = end + 1;
    }
    return true;
}

class Routing {
    struct Change {
        unsigned stream, client, original, current;
    };
    int connection;
    std::vector<Change> changes;

public:
    explicit Routing(int fd) : connection(fd) {}
    ~Routing() { restore(); }
    Routing(const Routing &) = delete;
    Routing &operator=(const Routing &) = delete;

    bool move(unsigned streamId, unsigned sinkId) {
        std::vector<Stream> current;
        if (!streams(connection, current)) return false;
        const Stream *found = nullptr;
        for (const auto &stream : current) if (stream.id == streamId) found = &stream;
        if (!found) return false;
        Change *change = nullptr;
        for (auto &candidate : changes)
            if (candidate.stream == streamId && candidate.client == found->client) change = &candidate;
        if (!change && changes.size() == 32) return false;
        const unsigned original = found->sink;
        const unsigned client = found->client;
        std::string output;
        if (!pactl({"move-sink-input", std::to_string(streamId), std::to_string(sinkId)}, connection, output)) {
            std::fprintf(stderr, "AudioX: could not route stream: %s\n", output.c_str());
            return false;
        }
        if (change) {
            if (original != change->current) change->original = original;
            change->current = sinkId;
        }
        else changes.push_back({streamId, client, original, sinkId});
        return true;
    }

    void restore() {
        if (changes.empty()) return;
        std::vector<Stream> current;
        if (!streams(connection, current)) {
            std::fprintf(stderr, "AudioX: could not inspect streams for route restoration.\n");
            changes.clear();
            return;
        }
        for (const auto &change : changes) {
            for (const auto &stream : current) {
                if (stream.id != change.stream || stream.client != change.client || stream.sink != change.current)
                    continue;
                std::string output;
                if (!pactl({"move-sink-input", std::to_string(change.stream),
                            std::to_string(change.original)}, connection, output))
                    std::fprintf(stderr, "AudioX: could not restore stream %u: %s\n", change.stream, output.c_str());
                break;
            }
        }
        changes.clear();
    }
};

} // namespace desktop
