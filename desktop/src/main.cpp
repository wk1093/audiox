#include "../../include/http/websocket.hpp"
#include "../../include/desktop/routing.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <thread>
#include <vector>

#if defined(AUDIOX_DESKTOP_LINUX) && !defined(__linux__)
#error "The Linux desktop target requires a Linux compiler."
#elif defined(AUDIOX_DESKTOP_WINDOWS) && (!defined(_WIN64) || !defined(__x86_64__))
#error "The Windows desktop target requires an x86-64 Windows compiler."
#elif defined(AUDIOX_DESKTOP_MACOS) && !defined(__APPLE__)
#error "The macOS desktop target requires a macOS compiler."
#endif

#ifdef _WIN32
using Socket = SOCKET;
constexpr Socket invalidSocket = INVALID_SOCKET;
static void closeSocket(Socket fd) { closesocket(fd); }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include "routing.hpp"
#endif
using Socket = int;
constexpr Socket invalidSocket = -1;
static void closeSocket(Socket fd) { close(fd); }
#endif

namespace {

#ifndef _WIN32
volatile sig_atomic_t stopping = 0;
void stopSignal(int) { stopping = 1; }
#endif

bool setNonBlocking(Socket fd, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(fd, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(fd, F_GETFL, 0);
    return flags >= 0 && fcntl(fd, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0;
#endif
}

int waitSocket(Socket fd, bool writing, int seconds) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(fd, &set);
    timeval timeout{seconds, 0};
    return select(static_cast<int>(fd) + 1, writing ? nullptr : &set,
                  writing ? &set : nullptr, nullptr, &timeout);
}

bool transfer(Socket fd, void *buffer, size_t length, bool writing) {
    auto *bytes = static_cast<char *>(buffer);
    while (length) {
        if (waitSocket(fd, writing, 5) <= 0) return false;
        const int count = writing ? send(fd, bytes, static_cast<int>(length), 0) :
                                    recv(fd, bytes, static_cast<int>(length), 0);
        if (count <= 0) return false;
        bytes += count;
        length -= static_cast<size_t>(count);
    }
    return true;
}

Socket connectTo(const char *host, unsigned port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host, &address.sin_addr) != 1) return invalidSocket;
    const Socket fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd == invalidSocket) return fd;
    if (!setNonBlocking(fd, true)) { closeSocket(fd); return invalidSocket; }
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
        if (waitSocket(fd, true, 3) <= 0) { closeSocket(fd); return invalidSocket; }
        int error = 0;
#ifdef _WIN32
        int length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, reinterpret_cast<char *>(&error), &length) != 0 || error) {
#else
        socklen_t length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error) {
#endif
            closeSocket(fd);
            return invalidSocket;
        }
    }
    if (!setNonBlocking(fd, false)) { closeSocket(fd); return invalidSocket; }
    return fd;
}

std::string base64(const uint8_t *bytes, size_t length) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (size_t i = 0; i < length; i += 3) {
        const uint32_t value = (static_cast<uint32_t>(bytes[i]) << 16) |
            (i + 1 < length ? static_cast<uint32_t>(bytes[i + 1]) << 8 : 0) |
            (i + 2 < length ? bytes[i + 2] : 0);
        out += alphabet[(value >> 18) & 63];
        out += alphabet[(value >> 12) & 63];
        out += i + 1 < length ? alphabet[(value >> 6) & 63] : '=';
        out += i + 2 < length ? alphabet[value & 63] : '=';
    }
    return out;
}

void randomBytes(uint8_t *bytes, size_t length) {
    std::random_device source;
    for (size_t i = 0; i < length; ++i) bytes[i] = static_cast<uint8_t>(source());
}

bool upgrade(Socket fd) {
    uint8_t nonce[16];
    randomBytes(nonce, sizeof(nonce));
    const std::string key = base64(nonce, sizeof(nonce));
    char expected[32];
    if (!audiox::websocket::acceptKey(key.c_str(), expected, sizeof(expected))) return false;
    std::string request = "GET /ws/desktop HTTP/1.1\r\nHost: audiox.local\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
        "Sec-WebSocket-Protocol: audiox.desktop.v1\r\nSec-WebSocket-Key: " + key + "\r\n\r\n";
    if (!transfer(fd, request.data(), request.size(), true)) return false;
    std::string response;
    while (response.size() < 8192 && response.find("\r\n\r\n") == std::string::npos) {
        char ch;
        if (!transfer(fd, &ch, 1, false)) return false;
        response += ch;
    }
    if (response.find("\r\n\r\n") == std::string::npos ||
        response.compare(0, 13, "HTTP/1.1 101 ") != 0) return false;
    bool accept = false, protocol = false, connection = false, upgradeHeader = false;
    size_t pos = response.find("\r\n") + 2;
    while (pos < response.size()) {
        const size_t end = response.find("\r\n", pos);
        if (end == pos || end == std::string::npos) break;
        const size_t colon = response.find(':', pos);
        if (colon == std::string::npos || colon >= end) return false;
        std::string name = response.substr(pos, colon - pos);
        std::transform(name.begin(), name.end(), name.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        size_t start = colon + 1;
        while (start < end && (response[start] == ' ' || response[start] == '\t')) ++start;
        const std::string value = response.substr(start, end - start);
        if (name == "sec-websocket-accept") accept = value == expected;
        else if (name == "sec-websocket-protocol") protocol = value == "audiox.desktop.v1";
        else if (name == "connection") connection = value == "Upgrade" || value == "upgrade";
        else if (name == "upgrade") upgradeHeader = value == "websocket";
        pos = end + 2;
    }
    return accept && protocol && connection && upgradeHeader;
}

bool sendFrame(Socket fd, uint8_t opcode, const std::string &message) {
    if (message.size() > 1024) return false;
    uint8_t mask[4];
    randomBytes(mask, sizeof(mask));
    std::vector<uint8_t> frame;
    frame.push_back(static_cast<uint8_t>(0x80 | opcode));
    if (message.size() < 126) frame.push_back(static_cast<uint8_t>(0x80 | message.size()));
    else {
        frame.push_back(0x80 | 126);
        frame.push_back(static_cast<uint8_t>(message.size() >> 8));
        frame.push_back(static_cast<uint8_t>(message.size()));
    }
    frame.insert(frame.end(), mask, mask + sizeof(mask));
    for (size_t i = 0; i < message.size(); ++i)
        frame.push_back(static_cast<uint8_t>(message[i]) ^ mask[i & 3]);
    return transfer(fd, frame.data(), frame.size(), true);
}

bool receiveFrame(Socket fd, uint8_t *opcode, std::string *payload) {
    uint8_t header[2];
    if (!transfer(fd, header, sizeof(header), false) ||
        (header[0] & 0xf0) != 0x80 || (header[1] & 0x80)) return false;
    size_t length = header[1] & 0x7f;
    if (length == 126) {
        uint8_t extended[2];
        if (!transfer(fd, extended, sizeof(extended), false)) return false;
        length = (static_cast<size_t>(extended[0]) << 8) | extended[1];
        if (length < 126) return false;
    } else if (length == 127) return false;
    *opcode = header[0] & 0x0f;
    if (length > 1024 || ((*opcode & 0x08) && length > 125)) return false;
    payload->resize(length);
    return !length || transfer(fd, payload->data(), length, false);
}

#ifdef __linux__
bool hasExecutable(const char *name) {
    const char *path = std::getenv("PATH");
    if (!path) return false;
    std::string paths(path);
    size_t start = 0;
    do {
        const size_t end = paths.find(':', start);
        const std::string dir = paths.substr(start, end == std::string::npos ? end : end - start);
        if (!dir.empty() && access((dir + "/" + name).c_str(), X_OK) == 0) return true;
        if (end == std::string::npos) break;
        start = end + 1;
    } while (start <= paths.size());
    return false;
}

std::string mediaStatus(Socket connection) {
    int output[2];
    if (pipe2(output, O_CLOEXEC | O_NONBLOCK) != 0) return "error";
    const pid_t child = fork();
    if (child < 0) {
        close(output[0]);
        close(output[1]);
        return "error";
    }
    if (child == 0) {
        closeSocket(connection);
        close(output[0]);
        // Keep the command's output blocking; only the reader needs nonblocking I/O.
        const int flags = fcntl(output[1], F_GETFL, 0);
        if (flags < 0 || fcntl(output[1], F_SETFL, flags & ~O_NONBLOCK) < 0 ||
            dup2(output[1], STDOUT_FILENO) < 0 || dup2(output[1], STDERR_FILENO) < 0)
            _exit(127);
        close(output[1]);
        if (setenv("LC_ALL", "C", 1) != 0) _exit(127);
        execlp("playerctl", "playerctl", "--all-players", "status", static_cast<char *>(nullptr));
        _exit(127);
    }
    close(output[1]);
    std::string text;
    bool eof = false, exited = false, failed = false;
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!failed && !(eof && exited)) {
        char buffer[512];
        for (;;) {
            const ssize_t count = read(output[0], buffer, sizeof(buffer));
            if (count > 0) {
                text.append(buffer, static_cast<size_t>(count));
                if (text.size() > 4096) { failed = true; break; }
            } else if (count == 0) {
                eof = true;
                break;
            } else if (errno == EINTR) continue;
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
        pollfd fd{output[0], POLLIN, 0};
        const int result = poll(&fd, 1, 50);
        if (result < 0 && errno != EINTR) failed = true;
    }
    close(output[0]);
    if (!exited) {
        kill(child, SIGKILL);
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) {}
    }
    if (failed || !WIFEXITED(status)) return "error";
    if (WEXITSTATUS(status) != 0)
        return text == "No players found\n" ? "none" : "error";
    bool playing = false, paused = false, stopped = false;
    size_t start = 0;
    while (start < text.size()) {
        const size_t end = text.find('\n', start);
        const std::string line = text.substr(start, end == std::string::npos ? end : end - start);
        if (line == "Playing") playing = true;
        else if (line == "Paused") paused = true;
        else if (line == "Stopped") stopped = true;
        else return "error";
        if (end == std::string::npos) break;
        start = end + 1;
    }
    if (playing) return "playing";
    if (paused) return "paused";
    if (stopped) return "stopped";
    return "error";
}
#endif

std::string hello(const std::string &media) {
    char hostname[256]{};
    if (gethostname(hostname, sizeof(hostname) - 1) != 0) std::strcpy(hostname, "unknown");
    for (char *p = hostname; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) *p = '-';
    }
    hostname[64] = '\0';
    if (!hostname[0]) std::strcpy(hostname, "unknown");
    std::string capabilities;
#ifdef __linux__
    const char *os = "linux";
    const char *runtime = std::getenv("XDG_RUNTIME_DIR");
    const std::string fallback = "/run/user/" + std::to_string(getuid());
    const char *remote = std::getenv("PIPEWIRE_REMOTE");
    if (!remote || !*remote) remote = "pipewire-0";
    const std::string socketPath = std::string(runtime && *runtime ? runtime : fallback.c_str()) + "/" + remote;
    struct stat status{};
    if (stat(socketPath.c_str(), &status) == 0 && S_ISSOCK(status.st_mode)) {
        capabilities = "\"pipewire\"";
        if (hasExecutable("wpctl")) capabilities += ",\"default_sink\"";
        if (hasExecutable("pactl")) capabilities += ",\"route_stream\"";
    }
    if (!media.empty()) {
        if (!capabilities.empty()) capabilities += ',';
        capabilities += "\"mpris\"";
    }
#elif defined(_WIN32)
    const char *os = "windows";
    capabilities = "\"default_sink\"";
#else
    const char *os = "macos";
#endif
    std::string result = std::string("{\"type\":\"hello\",\"hostname\":\"") + hostname +
        "\",\"os\":\"" + os + "\",\"capabilities\":[" + capabilities + "]";
    if (!media.empty()) result += ",\"media\":\"" + media + "\"";
    return result + "}";
}

void runConnection(Socket fd, bool &connected) {
    if (!upgrade(fd)) return;
    std::string media;
    bool mediaNegotiated = false;
    std::string previous = hello(media);
    if (!sendFrame(fd, 0x1, previous)) return;
    connected = true;
    std::fprintf(stderr, "AudioX: desktop connected.\n");
#ifdef __linux__
    desktop::Routing routing(fd);
#endif
    auto nextCheck = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (true) {
#ifndef _WIN32
        if (stopping) return;
#endif
        const int ready = waitSocket(fd, false, 1);
        if (ready < 0) return;
        if (ready > 0) {
            uint8_t opcode;
            std::string payload;
            if (!receiveFrame(fd, &opcode, &payload)) return;
            if (opcode == 0x8) return;
            if (opcode == 0x9) {
                if (!sendFrame(fd, 0xA, payload)) return;
            } else if (opcode == 0x1) {
                if (payload == "{\"type\":\"ready\",\"protocol\":\"audiox.desktop.v1\",\"media_status\":true}") {
                    mediaNegotiated = true;
                    nextCheck = std::chrono::steady_clock::now();
                }
#ifdef __linux__
                else {
                    desktop::RouteCommand command;
                    if (desktop::parseRouteCommand(payload, command)) {
                        const bool success = routing.move(command.stream, command.sink);
                        const std::string response = desktop::routeResult(command.id, success);
                        if (!sendFrame(fd, 0x1, response)) return;
                    }
                }
#endif
            } else if (opcode != 0xA) return;
        }
        if (std::chrono::steady_clock::now() >= nextCheck) {
#ifdef __linux__
            const std::string nextMedia = mediaNegotiated && hasExecutable("playerctl") ? mediaStatus(fd) : "";
            if (nextMedia == "error" && media != "error")
                std::fprintf(stderr, "AudioX: unable to query MPRIS playback status.\n");
            media = nextMedia;
#endif
            (void)mediaNegotiated;
            const std::string current = hello(media);
            if (current != previous) {
                if (!sendFrame(fd, 0x1, current)) return;
                previous = current;
            }
            nextCheck = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    const char *host = "172.31.73.1";
    unsigned port = 80;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--help")) {
            std::puts("Usage: audiox-companion [--host IPv4] [--port PORT]");
            return 0;
        }
        if (!std::strcmp(argv[i], "--host") && i + 1 < argc) host = argv[++i];
        else if (!std::strcmp(argv[i], "--port") && i + 1 < argc) {
            char *end = nullptr;
            const unsigned long value = std::strtoul(argv[++i], &end, 10);
            if (!*argv[i] || *end || !value || value > 65535) {
                std::fprintf(stderr, "AudioX: invalid port.\n");
                return 1;
            }
            port = static_cast<unsigned>(value);
        } else {
            std::fprintf(stderr, "AudioX: invalid argument (see --help).\n");
            return 1;
        }
    }
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        std::fprintf(stderr, "AudioX: could not initialize networking.\n");
        return 1;
    }
#else
    signal(SIGPIPE, SIG_IGN);
    signal(SIGTERM, stopSignal);
    signal(SIGINT, stopSignal);
#endif
    in_addr address{};
    if (inet_pton(AF_INET, host, &address) != 1) {
        std::fprintf(stderr, "AudioX: host must be an IPv4 address.\n");
        return 1;
    }
    unsigned delay = 1;
    bool reported = false;
    while (
#ifndef _WIN32
        !stopping
#else
        true
#endif
    ) {
        const Socket fd = connectTo(host, port);
        if (fd != invalidSocket) {
            const auto connectedAt = std::chrono::steady_clock::now();
            bool connected = false;
            runConnection(fd, connected);
            if (connected) reported = false;
            closeSocket(fd);
            if (std::chrono::steady_clock::now() - connectedAt >= std::chrono::seconds(10)) delay = 1;
        }
        if (!reported) {
            std::fprintf(stderr, "AudioX: disconnected or WebSocket unavailable; retrying.\n");
            reported = true;
        }
#ifndef _WIN32
        if (stopping) break;
#endif
        std::this_thread::sleep_for(std::chrono::seconds(delay));
        delay = std::min(delay * 2, 5U);
    }
}
