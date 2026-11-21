#include "http/context.hpp"
#include "http/websocket.hpp"
#include "desktop/routing.hpp"
#include "desktop/json.hpp"
#include "desktop/config.hpp"
#include <limits.h>
#include "http/ffmpeg.hpp"
#include "audio/context.hpp"
#include "system.hpp"
#include "init.hpp"
#include "update_archive.hpp"
#include "config/backup.hpp"
#include "config/paths.hpp"
#include "config/file.hpp"
#include "json.hpp"
#include "watchdog_heartbeat.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <atomic>
#include <array>
#include <deque>
#include <mutex>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/sendfile.h>

namespace {
constexpr const char *eventTopics[] = {
    "audio.meters", "midi.note", "midi.cc", "system.info", "routing.things", "desktop.status"
};
constexpr uint64_t eventIntervals[] = {50, 50, 50, 1000, 1000, 0};
constexpr size_t eventCount = sizeof(eventTopics) / sizeof(eventTopics[0]);
constexpr size_t desktopTopic = eventCount - 1;
}

struct HttpExchange {
    std::string header, body, response;
    size_t inputOffset{};
    int upload{-1};
    int download{-1};
    off_t downloadSize{};
    bool soundImport{};
    bool api{};
    std::atomic<bool> complete{false};
    std::atomic<bool> disconnected{false};

    ~HttpExchange() {
        if (upload >= 0) close(upload);
        if (download >= 0) close(download);
    }
};

namespace {
// Only the control thread sets this during dispatch. Direct socket-based tests
// and the network thread's protocol errors still use the normal socket helpers.
thread_local HttpExchange *exchange = nullptr;

ssize_t readRequest(int fd, void *out, size_t size) {
    if (!exchange) return recv(fd, out, size, 0);
    if (exchange->inputOffset < exchange->header.size()) {
        const size_t n = std::min(size, exchange->header.size() - exchange->inputOffset);
        memcpy(out, exchange->header.data() + exchange->inputOffset, n);
        exchange->inputOffset += n;
        return static_cast<ssize_t>(n);
    }
    if (exchange->upload >= 0) return read(exchange->upload, out, size);
    const size_t offset = exchange->inputOffset - exchange->header.size();
    if (offset >= exchange->body.size()) return 0;
    const size_t n = std::min(size, exchange->body.size() - offset);
    memcpy(out, exchange->body.data() + offset, n);
    exchange->inputOffset += n;
    return static_cast<ssize_t>(n);
}

ssize_t writeResponse(int fd, const void *data, size_t size) {
    if (!exchange) return send(fd, data, size, MSG_NOSIGNAL);
    exchange->response.append(static_cast<const char *>(data), size);
    return static_cast<ssize_t>(size);
}
}

struct HttpTransport {
    struct SoundJob {
        std::shared_ptr<HttpExchange> request;
        pid_t pid{-1};
        uint64_t deadline{};
        std::string temporary, destination;
        bool timedOut{};
    } sound;
    std::atomic<bool> stopping{false};
    pthread_t thread{};
    bool running{};
    std::mutex mutex;
    std::deque<std::shared_ptr<HttpExchange>> requests;
    std::array<std::shared_ptr<const std::string>, eventCount> events;
    std::atomic<uint32_t> demand{0};
    uint64_t nextEvent[eventCount]{};
    int rebootCommand{};
    uint64_t rebootAt{};
    bool desktopCanRoute{};
    bool desktopCanDiscover{};
    bool desktopCanAutoRoute{};
    bool desktopCanDefault{};
    bool desktopConnected{};
    uint32_t desktopSession{};
    bool routePending{};
    bool routeSent{};
    uint32_t routeSequence{};
    desktop::RouteCommand route{};
    uint64_t routeDeadline{};
    DesktopRouteState routeState{DesktopRouteState::Unknown};
    bool defaultWatchSent{}, defaultKnown{}, defaultPending{}, defaultSent{}, defaultUserRequested{};
    uint32_t defaultSequence{}, defaultId{};
    uint64_t defaultDeadline{}, defaultEventSerial{};
    std::string defaultName, defaultRequested;
    DesktopRouteState defaultState{DesktopRouteState::Unknown};
    std::deque<DesktopDefaultEvent> defaultEvents;
    bool inventoryPending{};
    bool inventorySent{};
    uint32_t inventorySequence{};
    uint32_t inventoryId{};
    uint64_t inventoryDeadline{};
    std::string inventory;
    DesktopInventoryState inventoryState{DesktopInventoryState::Unknown};
    std::string rules;
    bool rulesValid{};
    bool rulesPending{}, rulesSent{};
    uint32_t rulesId{};
    size_t rulesOffset{}, rulesExpected{};
    uint64_t rulesDeadline{};
    DesktopRulesState rulesState{DesktopRulesState::Unavailable};
    int autoStatus{-1};
};

#define HTTP_REQ_MAX 65536
#define HTTP_HEADER_MAX 16384
#define HTTP_BODY_MAX 49152
#define HTTP_INITRAM_PATH "/api/initram"
#define HTTP_INITRAM_MAX (64u * 1024u * 1024u)
#define HTTP_BOOT_LOADER_PATH "/api/boot/loader"
#define HTTP_BOOT_LOADER_FILE "/boot/initramfs.cpio.gz"
#define HTTP_BOOT_KERNEL_PATH "/api/boot/kernel"
#define HTTP_BOOT_DIR "/boot"
#define HTTP_BOOT_KERNEL_TEMP_FILE HTTP_BOOT_DIR "/audiox-kernel.update"
#define HTTP_BOOT_UPLOAD_MAX (150u * 1024u * 1024u)
#define HTTP_BOOT_CONFIG_FILE "/boot/config.txt"
#define HTTP_BOOT_CMDLINE_FILE "/boot/cmdline.txt"
#define HTTP_UPDATE_PATH "/api/update"
#define HTTP_UPDATE_MAX (256u * 1024u * 1024u)
#define HTTP_INITRAM_RCV_TIMEOUT_SEC 10
#define HTTP_ROOTFS_PREFIX "/api/rootfs/"
#define HTTP_ROOTFS_UPLOAD_MAX (150u * 1024u * 1024u)

#define HTTP_API_PREFIX "/api/"
#define HTTP_DEFAULT_HTML_PATH "/etc/www/index.html"

#define HTTP_STAGE_DIR ROOT_MOUNT_POINT "/staging"
#define HTTP_STAGE_INITRAMFS_PATH PROGRAM_STAGED_PATH
#define HTTP_STAGE_INITRAMFS_PART_PATH PROGRAM_STAGED_PATH ".part"
#define HTTP_STAGE_UPDATE_PATH HTTP_STAGE_DIR "/update.axu.part"

namespace {

struct HttpRequestHeaders {
    size_t contentLength{};
    bool hasContentLength{};
    bool hasHost{};
    bool hasUpgrade{};
    bool hasConnection{};
    bool hasWebSocketKey{};
    bool hasWebSocketVersion{};
    bool hasWebSocketProtocol{};
    bool hasOrigin{};
    bool hasFetchSite{};
    char host[256]{};
    char upgrade[64]{};
    char connection[128]{};
    char websocketKey[64]{};
    char websocketVersion[8]{};
    char websocketProtocol[64]{};
    char origin[256]{};
    char fetchSite[32]{};
};

static int setNonBlocking(int fd);
static bool allowedRequestHost(const char *host);
static bool queueWebSocketFrame(HttpWebSocketClient *client, uint8_t opcode,
                                const void *payload, size_t length);
static void closeWebSocket(HttpWebSocketClient *client);

static uint64_t httpMonotonicMs() {
    timespec ts{};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return static_cast<uint64_t>(ts.tv_sec) * 1000U +
        static_cast<uint64_t>(ts.tv_nsec) / 1000000U;
}

static bool hasToken(const char *list, const char *token) {
    if (!list || !token) return false;
    const size_t tokenLength = strlen(token);
    for (const char *p = list; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') ++p;
        const char *end = strchr(p, ',');
        size_t length = end ? static_cast<size_t>(end - p) : strlen(p);
        while (length && (p[length - 1] == ' ' || p[length - 1] == '\t')) --length;
        if (length == tokenLength && !strncasecmp(p, token, length)) return true;
        p = end ? end + 1 : p + length;
    }
    return false;
}

static bool sameOriginHost(const char *origin, const char *host) {
    if (!origin || !*origin || !host || !*host) return false;
    static constexpr char prefix[] = "http://";
    if (strncasecmp(origin, prefix, sizeof(prefix) - 1)) return false;
    const char *originHost = origin + sizeof(prefix) - 1;
    if (strchr(originHost, '/') || strchr(originHost, '?') || strchr(originHost, '#')) return false;
    if (!strcasecmp(originHost, host)) return true;
    const size_t hostLength = strlen(host);
    return hostLength > 3 && !strcasecmp(host + hostLength - 3, ":80") &&
        strlen(originHost) == hostLength - 3 && !strncasecmp(originHost, host, hostLength - 3);
}

static bool queueWebSocketFrame(HttpWebSocketClient *client, uint8_t opcode,
                                const void *payload, size_t length) {
    if (!client || client->fd < 0 || client->outputOffset < client->outputSize ||
        !audiox::websocket::encodeServerFrame(opcode, payload, length,
            client->output, sizeof(client->output), &client->outputSize)) return false;
    client->outputOffset = 0;
    return true;
}

static bool sendUpgradeResponse(int fd, const char *accept, const char *protocol) {
    char response[256];
    const int length = snprintf(response, sizeof(response),
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Accept: %s\r\n"
        "Sec-WebSocket-Protocol: %s\r\n\r\n", accept, protocol);
    if (length < 0 || static_cast<size_t>(length) >= sizeof(response)) return false;
    size_t sent = 0;
    while (sent < static_cast<size_t>(length)) {
        const ssize_t count = send(fd, response + sent, static_cast<size_t>(length) - sent,
                                   MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

static bool addWebSocketClient(HttpServer *server, int fd, const HttpRequestHeaders &headers,
                               bool desktop = false) {
    const char *protocol = desktop ? "audiox.desktop.v1" : "audiox.v1";
    if (!server || !hasToken(headers.connection, "upgrade") ||
        strcasecmp(headers.upgrade, "websocket") ||
        strcmp(headers.websocketVersion, "13") ||
        !hasToken(headers.websocketProtocol, protocol) ||
        !allowedRequestHost(headers.host) ||
        (desktop && (headers.hasOrigin || headers.hasFetchSite)) ||
        (headers.hasOrigin && !sameOriginHost(headers.origin, headers.host)) ||
        (headers.hasContentLength && headers.contentLength != 0)) return false;
    char accept[32];
    if (!audiox::websocket::acceptKey(headers.websocketKey, accept, sizeof(accept))) return false;
    HttpWebSocketClient *client = nullptr;
    if (desktop) {
        if (server->desktopClient.fd < 0) client = &server->desktopClient;
    } else {
        for (auto &candidate : server->websocketClients) {
            if (candidate.fd < 0) {
                client = &candidate;
                break;
            }
        }
    }
    if (!client || setNonBlocking(fd) < 0 || !sendUpgradeResponse(fd, accept, protocol)) return false;
    *client = HttpWebSocketClient{};
    client->fd = fd;
    client->connectedAtMs = client->lastActivityMs = httpMonotonicMs();
    static constexpr char ready[] = "{\"type\":\"ready\",\"protocol\":\"audiox.v1\"}";
    static constexpr char desktopReady[] = "{\"type\":\"ready\",\"protocol\":\"audiox.desktop.v1\",\"media_status\":true}";
    const char *message = desktop ? desktopReady : ready;
    if (!queueWebSocketFrame(client, 0x1, message, strlen(message))) {
        closeWebSocket(client);
        return false;
    }
    return true;
}

struct SubscriptionParser {
    const char *cursor;
    const char *end;

    void whitespace() { while (cursor < end && (*cursor == ' ' || *cursor == '\t' || *cursor == '\r' || *cursor == '\n')) ++cursor; }
    bool character(char value) { whitespace(); if (cursor == end || *cursor != value) return false; ++cursor; return true; }
    bool string(const char **value, size_t *length) {
        whitespace();
        if (cursor == end || *cursor++ != '"') return false;
        const char *begin = cursor;
        while (cursor < end && *cursor != '"') {
            const unsigned char c = static_cast<unsigned char>(*cursor);
            if (c < 0x20 || c == '\\' || c >= 0x7f) return false;
            ++cursor;
        }
        if (cursor == end) return false;
        *value = begin;
        *length = static_cast<size_t>(cursor - begin);
        ++cursor;
        return true;
    }
};

static bool parseWebSocketSubscription(const uint8_t *data, size_t length, bool *subscribe,
                                      uint32_t *topics) {
    if (!data || !length || length > 1024 || !subscribe || !topics) return false;
    SubscriptionParser parser{reinterpret_cast<const char *>(data),
                              reinterpret_cast<const char *>(data) + length};
    if (!parser.character('{')) return false;
    bool hasType = false, hasTopics = false;
    uint32_t mask = 0;
    for (unsigned field = 0; field < 2; ++field) {
        const char *key;
        size_t keyLength;
        if (!parser.string(&key, &keyLength) || !parser.character(':')) return false;
        if (keyLength == 4 && !memcmp(key, "type", 4)) {
            if (hasType) return false;
            const char *value;
            size_t valueLength;
            if (!parser.string(&value, &valueLength)) return false;
            if (valueLength == 9 && !memcmp(value, "subscribe", 9)) *subscribe = true;
            else if (valueLength == 11 && !memcmp(value, "unsubscribe", 11)) *subscribe = false;
            else return false;
            hasType = true;
        } else if (keyLength == 6 && !memcmp(key, "topics", 6)) {
            if (hasTopics || !parser.character('[')) return false;
            hasTopics = true;
            bool first = true;
            while (!parser.character(']')) {
                if (!first && !parser.character(',')) return false;
                first = false;
                const char *value;
                size_t valueLength;
                if (!parser.string(&value, &valueLength)) return false;
                bool known = false;
                for (unsigned i = 0; i < eventCount; ++i) {
                    if (valueLength == strlen(eventTopics[i]) && !memcmp(value, eventTopics[i], valueLength)) {
                        mask |= 1U << i;
                        known = true;
                    }
                }
                if (!known) return false;
            }
        } else {
            return false;
        }
        if (field == 0 && !parser.character(',')) return false;
    }
    if (!hasType || !hasTopics || !parser.character('}')) return false;
    parser.whitespace();
    if (parser.cursor != parser.end) return false;
    *topics = mask;
    return true;
}

static bool desktopToken(const char *text, size_t length, size_t limit) {
    if (!length || length > limit) return false;
    for (size_t i = 0; i < length; ++i) {
        const char c = text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

static bool parseDesktopHello(const uint8_t *data, size_t length, DesktopMedia *media = nullptr,
                              bool *canRoute = nullptr, bool *canDiscover = nullptr,
                              bool *canAutoRoute = nullptr, bool *canDefault = nullptr) {
    if (!data || !length || length > 1024) return false;
    SubscriptionParser parser{reinterpret_cast<const char *>(data),
                              reinterpret_cast<const char *>(data) + length};
    if (!parser.character('{')) return false;
    unsigned fields = 0;
    bool closed = false;
    DesktopMedia parsedMedia = DesktopMedia::Unavailable;
    bool parsedRoute = false;
    bool parsedDiscover = false;
    bool parsedAutoRoute = false;
    bool parsedDefault = false;
    for (unsigned i = 0; i < 5; ++i) {
        const char *key, *value;
        size_t keyLength, valueLength;
        if (parser.character('}')) { closed = true; break; }
        if (i && !parser.character(',')) return false;
        if (!parser.string(&key, &keyLength) || !parser.character(':')) return false;
        unsigned bit = 0;
        if (keyLength == 4 && !memcmp(key, "type", 4)) bit = 1;
        else if (keyLength == 8 && !memcmp(key, "hostname", 8)) bit = 2;
        else if (keyLength == 2 && !memcmp(key, "os", 2)) bit = 4;
        else if (keyLength == 12 && !memcmp(key, "capabilities", 12)) bit = 8;
        else if (keyLength == 5 && !memcmp(key, "media", 5)) bit = 16;
        else return false;
        if (fields & bit) return false;
        fields |= bit;
        if (bit == 8) {
            if (!parser.character('[')) return false;
            unsigned count = 0;
            while (!parser.character(']')) {
                if (count && !parser.character(',')) return false;
                if (++count > 16 || !parser.string(&value, &valueLength) ||
                    !desktopToken(value, valueLength, 32)) return false;
                if (valueLength == 12 && !memcmp(value, "route_stream", 12)) parsedRoute = true;
                if (valueLength == 14 && !memcmp(value, "pipewire_nodes", 14)) parsedDiscover = true;
                if (valueLength == 10 && !memcmp(value, "auto_route", 10)) parsedAutoRoute = true;
                if (valueLength == 12 && !memcmp(value, "default_sink", 12)) parsedDefault = true;
            }
        } else {
            if (!parser.string(&value, &valueLength)) return false;
            if (bit == 1) {
                if (valueLength != 5 || memcmp(value, "hello", 5)) return false;
            } else if (bit == 16) {
                if (valueLength == 7 && !memcmp(value, "playing", 7)) parsedMedia = DesktopMedia::Playing;
                else if (valueLength == 6 && !memcmp(value, "paused", 6)) parsedMedia = DesktopMedia::Paused;
                else if (valueLength == 7 && !memcmp(value, "stopped", 7)) parsedMedia = DesktopMedia::Stopped;
                else if (valueLength == 4 && !memcmp(value, "none", 4)) parsedMedia = DesktopMedia::None;
                else if (valueLength == 5 && !memcmp(value, "error", 5)) parsedMedia = DesktopMedia::Error;
                else return false;
            } else if (!desktopToken(value, valueLength, bit == 2 ? 64 : 32)) return false;
        }
    }
    if ((fields & 15) != 15) return false;
    if (!closed && !parser.character('}')) return false;
    parser.whitespace();
    if (parser.cursor != parser.end) return false;
    if (media) *media = parsedMedia;
    if (canRoute) *canRoute = parsedRoute;
    if (canDiscover) *canDiscover = parsedDiscover;
    if (canAutoRoute) *canAutoRoute = parsedAutoRoute;
    if (canDefault) *canDefault = parsedDefault;
    return true;
}

static void publishDesktopStatus(HttpServer *server, const uint8_t *hello = nullptr, size_t length = 0) {
    std::lock_guard<std::mutex> lock(server->transport->mutex);
    std::string message = "{\"type\":\"event\",\"topic\":\"desktop.status\",\"data\":{";
    if (hello) {
        message += "\"connected\":true,\"session\":" + std::to_string(server->transport->desktopSession) +
            ",\"hello\":";
        message.append(reinterpret_cast<const char *>(hello), length);
    } else {
        message += "\"connected\":false";
    }
    message += "}}";
    server->transport->events[desktopTopic] = std::make_shared<const std::string>(std::move(message));
}

static bool recordDesktopDefault(HttpTransport &transport, const std::string &name) {
    if (transport.defaultKnown && transport.defaultName == name) return true;
    if (transport.defaultEvents.size() >= 64) {
        printf("[HTTP] [WARN] Desktop default-sink event queue full; disconnecting companion\n");
        return false;
    }
    DesktopDefaultEvent event{};
    if (!transport.defaultKnown || transport.defaultName.empty() != name.empty())
        printf("[HTTP] Desktop default-sink snapshot: %s\n", name.empty() ? "unavailable" : "available");
    event.serial = ++transport.defaultEventSerial;
    event.session = transport.desktopSession;
    event.initial = !transport.defaultKnown || transport.defaultName.empty();
    snprintf(event.name, sizeof(event.name), "%s", name.c_str());
    transport.defaultEvents.push_back(event);
    transport.defaultName = name;
    transport.defaultKnown = true;
    return true;
}

static void closeWebSocket(HttpWebSocketClient *client) {
    if (client->fd >= 0) close(client->fd);
    *client = HttpWebSocketClient{};
}

static bool flushWebSocket(HttpWebSocketClient *client) {
    while (client->outputOffset < client->outputSize) {
        const ssize_t sent = send(client->fd, client->output + client->outputOffset,
            client->outputSize - client->outputOffset, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (sent < 0 && errno == EINTR) continue;
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        if (sent <= 0) return false;
        client->outputOffset += static_cast<size_t>(sent);
    }
    client->outputOffset = client->outputSize = 0;
    return true;
}

static bool receiveWebSocket(HttpWebSocketClient *client, uint64_t now, HttpServer *desktopServer = nullptr) {
    for (;;) {
        if (client->inputSize == sizeof(client->input)) return false;
        const ssize_t received = recv(client->fd, client->input + client->inputSize,
            sizeof(client->input) - client->inputSize, MSG_DONTWAIT);
        if (received < 0 && errno == EINTR) continue;
        if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        if (received <= 0) return false;
        client->inputSize += static_cast<size_t>(received);
    }
    size_t consumed = 0;
    while (consumed < client->inputSize) {
        audiox::websocket::Frame frame{};
        const auto result = audiox::websocket::decodeClientFrameResult(
            client->input + consumed, client->inputSize - consumed, &frame);
        if (result == audiox::websocket::FrameDecodeResult::Incomplete) break;
        if (result == audiox::websocket::FrameDecodeResult::Invalid) return false;
        uint8_t payload[1024];
        memcpy(payload, frame.payload, frame.length);
        const size_t headerLength = frame.consumed - frame.length;
        audiox::websocket::unmask(payload, frame.length, client->input + consumed + headerLength - 4);
        consumed += frame.consumed;
        client->lastActivityMs = now;
        if (frame.opcode == 0x8) {
            if (frame.length == 1) return false;
            static constexpr uint8_t closePayload[] = {0x03, 0xE8};
            if (!queueWebSocketFrame(client, 0x8, closePayload, sizeof(closePayload))) return false;
            client->closeAfterFlush = true;
            return true;
        }
        if (frame.opcode == 0x9) {
            if (!queueWebSocketFrame(client, 0xA, payload, frame.length)) return false;
        } else if (frame.opcode == 0xA) {
            client->pingAtMs = 0;
        } else if (frame.opcode == 0x1) {
            if (desktopServer) {
                const std::string_view desktopMessage(reinterpret_cast<const char *>(payload), frame.length);
                uint32_t defaultId;
                std::string defaultName;
                bool defaultDone;
                const bool defaultSnapshot = desktop::parseDefaultMessage(
                    desktopMessage, "default_snapshot", defaultId, defaultName, defaultDone);
                const bool defaultSuccess = !defaultSnapshot && desktop::parseDefaultMessage(
                    desktopMessage, "default_result", defaultId, defaultName, defaultDone);
                const bool defaultFailure = !defaultSnapshot && !defaultSuccess && desktop::parseDefaultMessage(
                    desktopMessage, "default_failed", defaultId, defaultName, defaultDone);
                if (defaultSnapshot || defaultSuccess || defaultFailure) {
                    std::lock_guard<std::mutex> lock(desktopServer->transport->mutex);
                    auto &transport = *desktopServer->transport;
                    if (!client->desktopIdentified || !transport.desktopCanDefault ||
                        !transport.defaultWatchSent) return false;
                    if (defaultSnapshot) {
                        if (!recordDesktopDefault(transport, defaultName)) return false;
                    } else {
                        if (!transport.defaultPending || !transport.defaultSent ||
                            defaultId != transport.defaultId ||
                            (defaultSuccess && defaultName != transport.defaultRequested)) return false;
                        if (defaultSuccess) {
                            if (transport.defaultUserRequested) {
                                if (!recordDesktopDefault(transport, defaultName)) return false;
                            } else {
                                transport.defaultName = defaultName;
                                transport.defaultKnown = true;
                            }
                        } else printf("[HTTP] [WARN] Desktop default-sink request %u failed\n", defaultId);
                        transport.defaultState = defaultSuccess ? DesktopRouteState::Succeeded : DesktopRouteState::Failed;
                        transport.defaultPending = transport.defaultSent = false;
                    }
                    continue;
                }
                uint32_t resultId;
                bool resultOk;
                if (desktop::parseRouteResult(
                        std::string_view(reinterpret_cast<const char *>(payload), frame.length),
                        resultId, resultOk)) {
                    std::lock_guard<std::mutex> lock(desktopServer->transport->mutex);
                    auto &transport = *desktopServer->transport;
                    if (!transport.routePending || !transport.routeSent || transport.route.id != resultId)
                        return false;
                    if (!resultOk) printf("[HTTP] [WARN] Desktop route request %u failed\n", resultId);
                    transport.routeState = resultOk ? DesktopRouteState::Succeeded : DesktopRouteState::Failed;
                    transport.routePending = transport.routeSent = false;
                    continue;
                }
                uint32_t ackId, ackOffset;
                bool ackOk;
                bool autoOk;
                if (desktop::parseAutoStatus(
                        std::string_view(reinterpret_cast<const char *>(payload), frame.length), autoOk)) {
                    std::lock_guard<std::mutex> lock(desktopServer->transport->mutex);
                    if (!client->desktopIdentified || !desktopServer->transport->desktopCanAutoRoute) return false;
                    if (!desktopServer->transport->rulesPending)
                        desktopServer->transport->autoStatus = autoOk ? 1 : 0;
                    continue;
                }
                if (desktop::parseRulesAck(
                        std::string_view(reinterpret_cast<const char *>(payload), frame.length),
                        ackId, ackOffset, ackOk)) {
                    std::lock_guard<std::mutex> lock(desktopServer->transport->mutex);
                    auto &transport = *desktopServer->transport;
                    if (!transport.rulesPending || !transport.rulesSent || ackId != transport.rulesId ||
                        (ackOk && ackOffset != transport.rulesExpected)) return false;
                    transport.rulesSent = false;
                    if (!ackOk) {
                        printf("[HTTP] [WARN] Desktop rejected automatic routing rules\n");
                        transport.rulesPending = false;
                        transport.rulesState = DesktopRulesState::Failed;
                    } else {
                        transport.rulesOffset = ackOffset;
                        if (transport.rulesOffset == transport.rules.size()) {
                            transport.rulesPending = false;
                            transport.rulesState = DesktopRulesState::Succeeded;
                        }
                    }
                    continue;
                }
                uint32_t inventoryId, inventoryOffset;
                bool inventoryDone;
                std::string inventoryBytes;
                const std::string_view message(reinterpret_cast<const char *>(payload), frame.length);
                const bool inventoryError = desktop::parseInventoryError(message, inventoryId);
                const bool inventoryChunk = !inventoryError && desktop::parseInventoryChunk(
                    message, inventoryId, inventoryOffset, inventoryBytes, inventoryDone);
                if (inventoryError || inventoryChunk) {
                    std::lock_guard<std::mutex> lock(desktopServer->transport->mutex);
                    auto &transport = *desktopServer->transport;
                    if (!transport.inventoryPending || !transport.inventorySent ||
                        transport.inventoryId != inventoryId) return false;
                    transport.inventorySent = false;
                    if (inventoryError) {
                        printf("[HTTP] [WARN] Desktop discovery failed\n");
                        transport.inventory.clear();
                        transport.inventoryPending = false;
                        transport.inventoryState = DesktopInventoryState::Failed;
                    } else {
                        if (inventoryOffset != transport.inventory.size() ||
                            transport.inventory.size() + inventoryBytes.size() > 262145) return false;
                        transport.inventory += inventoryBytes;
                        if (inventoryDone) {
                            transport.inventoryPending = false;
                            desktop::JsonCursor parser(transport.inventory);
                            std::string_view value;
                            if (!parser.next(value) || !parser.finished() || value.empty() || value.front() != '[') {
                                printf("[HTTP] [WARN] Invalid desktop discovery response\n");
                                transport.inventory.clear();
                                transport.inventoryState = DesktopInventoryState::Failed;
                            } else transport.inventoryState = DesktopInventoryState::Succeeded;
                        }
                    }
                    continue;
                }
                DesktopMedia media;
                bool canRoute, canDiscover, canAutoRoute, canDefault;
                if (!parseDesktopHello(payload, frame.length, &media, &canRoute, &canDiscover, &canAutoRoute, &canDefault)) return false;
                {
                    std::lock_guard<std::mutex> lock(desktopServer->transport->mutex);
                    if (!client->desktopIdentified && ++desktopServer->transport->desktopSession == 0)
                        ++desktopServer->transport->desktopSession;
                    desktopServer->transport->desktopCanRoute = canRoute;
                    desktopServer->transport->desktopCanDiscover = canDiscover;
                    auto &transport = *desktopServer->transport;
                    if (!transport.desktopConnected || transport.desktopCanDefault != canDefault)
                        printf("[HTTP] Desktop default-sink capability: %s\n",
                               canDefault ? "available" : "not advertised");
                    transport.desktopConnected = true;
                    if (!canDefault && transport.desktopCanDefault) {
                        if (transport.defaultKnown && !recordDesktopDefault(transport, "")) return false;
                        transport.defaultKnown = false;
                        transport.defaultWatchSent = false;
                        if (transport.defaultPending) {
                            printf("[HTTP] [WARN] Desktop default-sink request cancelled: capability unavailable\n");
                            transport.defaultState = DesktopRouteState::Failed;
                        }
                        transport.defaultPending = transport.defaultSent = false;
                    }
                    transport.desktopCanDefault = canDefault;
                    if (canAutoRoute && !desktopServer->transport->desktopCanAutoRoute &&
                        desktopServer->transport->rulesValid) {
                        if (++desktopServer->transport->rulesId == 0) ++desktopServer->transport->rulesId;
                        desktopServer->transport->rulesOffset = 0;
                        desktopServer->transport->rulesPending = true;
                        desktopServer->transport->rulesSent = false;
                        desktopServer->transport->rulesState = DesktopRulesState::Pending;
                        desktopServer->transport->autoStatus = -1;
                    } else if (!canAutoRoute && desktopServer->transport->desktopCanAutoRoute) {
                        desktopServer->transport->rulesPending = desktopServer->transport->rulesSent = false;
                        desktopServer->transport->rulesState = DesktopRulesState::Unavailable;
                    }
                    desktopServer->transport->desktopCanAutoRoute = canAutoRoute;
                    if (!canRoute && desktopServer->transport->routePending &&
                        !desktopServer->transport->routeSent) {
                        printf("[HTTP] [WARN] Desktop route request cancelled: capability unavailable\n");
                        desktopServer->transport->routePending = false;
                        desktopServer->transport->routeState = DesktopRouteState::Failed;
                    }
                    if (!canDiscover && (desktopServer->transport->inventoryPending ||
                        desktopServer->transport->inventoryState == DesktopInventoryState::Succeeded)) {
                        printf("[HTTP] [WARN] Desktop discovery cancelled: capability unavailable\n");
                        desktopServer->transport->inventoryPending = false;
                        desktopServer->transport->inventorySent = false;
                        desktopServer->transport->inventoryState = DesktopInventoryState::Failed;
                        desktopServer->transport->inventory.clear();
                    }
                }
                desktopServer->desktopMedia.store(media);
                client->desktopIdentified = true;
                publishDesktopStatus(desktopServer, payload, frame.length);
                continue;
            }
            bool subscribe = false;
            uint32_t topics = 0;
            if (!parseWebSocketSubscription(payload, frame.length, &subscribe, &topics)) return false;
            if (subscribe) client->topics |= topics;
            else client->topics &= ~topics;
        } else {
            return false;
        }
    }
    if (consumed) {
        memmove(client->input, client->input + consumed, client->inputSize - consumed);
        client->inputSize -= consumed;
    }
    return true;
}

static int setSocketTimeouts(int fd, long recv_ms, long send_ms) {
    if (exchange) return 0;
    if (fd < 0) {
        return -1;
    }

    struct timeval recv_to;
    recv_to.tv_sec = recv_ms / 1000;
    recv_to.tv_usec = (recv_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_to, sizeof(recv_to)) < 0) {
        return -1;
    }

    struct timeval send_to;
    send_to.tv_sec = send_ms / 1000;
    send_to.tv_usec = (send_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_to, sizeof(send_to)) < 0) {
        return -1;
    }

    return 0;
}

static int sendErrnoResponse(HttpServer *server,
                             int cfd,
                             const char *status,
                             const char *prefix,
                             int saved_errno) {
    char body[256];
    int n = snprintf(body,
                     sizeof(body),
                     "%s: %s\n",
                     prefix ? prefix : "request failed",
                     strerror(saved_errno));
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n >= sizeof(body)) {
        n = (int)(sizeof(body) - 1);
    }
    return server->sendResponse(cfd,
                                status ? status : "500 Internal Server Error",
                                "text/plain; charset=utf-8",
                                body,
                                (size_t)n);
}

static int checkStageCapacity(HttpServer *server, int cfd, size_t content_length) {
    struct statvfs st;
    if (statvfs(ROOT_MOUNT_POINT, &st) < 0) {
        return 0;
    }

    unsigned long long free_bytes = (unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize;
    if (free_bytes >= (unsigned long long)content_length) {
        return 0;
    }

    char body[256];
    int n = snprintf(body,
                     sizeof(body),
                     "insufficient storage: need %lu bytes, have %llu bytes free on %s\n",
                     (unsigned long)content_length,
                     free_bytes,
                     ROOT_MOUNT_POINT);
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n >= sizeof(body)) {
        n = (int)(sizeof(body) - 1);
    }
    (void)server->sendResponse(cfd,
                               "507 Insufficient Storage",
                               "text/plain; charset=utf-8",
                               body,
                               (size_t)n);
    return -1;
}

static int writeAllToFd(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t written = 0;
    while (written < len) {
        audioxWatchdogHeartbeat();
        ssize_t nw = write(fd, p + written, len - written);
        if (nw < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nw == 0) {
            return -1;
        }
        written += (size_t)nw;
    }
    return 0;
}

static int streamRequestBodyToFd(int cfd,
                                 int fd,
                                 const char *initial_body,
                                 size_t initial_body_len,
                                 size_t content_length) {
    size_t written = 0;
    size_t initial_write = initial_body_len;
    if (initial_write > content_length) {
        initial_write = content_length;
    }
    if (initial_write > 0 && writeAllToFd(fd, initial_body, initial_write) < 0) {
        return -1;
    }
    written = initial_write;

    char buf[16384];
    while (written < content_length) {
        audioxWatchdogHeartbeat();
        size_t want = content_length - written;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }

        ssize_t nr = readRequest(cfd, buf, want);
        if (nr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nr == 0) {
            return 1;
        }
        if (writeAllToFd(fd, buf, (size_t)nr) < 0) {
            return -1;
        }
        written += (size_t)nr;
    }
    return 0;
}

static int mountBootPartition(void) {
    if (mkdir("/boot", 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    if (mount(BOOT_DEVICE_PATH, "/boot", "vfat", MS_NOATIME, NULL) < 0 && errno != EBUSY) {
        return -1;
    }
    return 0;
}

// Kernel file the installed config.txt boots; the boot partition must already be mounted.
static void currentBootKernelName(char *out, size_t out_sz) {
    char text[16384];
    size_t len = 0;
    int fd = open(HTTP_BOOT_CONFIG_FILE, O_RDONLY);
    if (fd >= 0) {
        ssize_t nr = read(fd, text, sizeof(text));
        if (nr > 0) {
            len = (size_t)nr;
        }
        close(fd);
    }
    audioxConfigKernelName(text, len, out, out_sz);
}

// Board family of the running hardware from the device-tree SoC, falling back to the build target.
static const char *detectBoard(void) {
    char compat[512];
    int fd = open("/proc/device-tree/compatible", O_RDONLY);
    if (fd >= 0) {
        ssize_t nr = read(fd, compat, sizeof(compat) - 1);
        close(fd);
        if (nr > 0) {
            for (ssize_t i = 0; i < nr; ++i) {
                if (compat[i] == '\0') {
                    compat[i] = '\n';
                }
            }
            compat[nr] = '\0';
            if (strstr(compat, "brcm,bcm2712")) {
                return "pi5";
            }
            if (strstr(compat, "brcm,bcm2711")) {
                return "pi4";
            }
        }
    }
#if defined(AUDIOX_BOARD)
    return AUDIOX_BOARD;
#else
    return "";
#endif
}

// Extracts a validated kernel file name from a "name=" query parameter.
static bool queryKernelName(const char *query, char *out, size_t out_sz) {
    if (!query) {
        return false;
    }
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, "name=", 5) == 0) {
            const char *v = p + 5;
            size_t len = strcspn(v, "&");
            if (len >= out_sz) {
                return false;
            }
            memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            ++p;
        }
    }
    return false;
}

static int handleBootFile(HttpServer *server,
                          int cfd,
                          const char *method,
                          const char *path,
                          const char *query,
                          size_t content_length,
                          const char *initial_body,
                          size_t initial_body_len) {
    char kernel_path[sizeof(HTTP_BOOT_DIR) + AUDIOX_UPDATE_VALUE_MAX + 1];
    const char *file_path = NULL;
    const bool is_kernel = strcmp(path, HTTP_BOOT_KERNEL_PATH) == 0;
    if (strcmp(path, HTTP_BOOT_LOADER_PATH) == 0) {
        file_path = HTTP_BOOT_LOADER_FILE;
    } else if (!is_kernel) {
        return -1;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "PUT") != 0) {
        static const char mna[] = "method not allowed\n";
        return server->sendResponse(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
    }
    if (mountBootPartition() < 0) {
        static const char err[] = "boot partition unavailable\n";
        return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    if (is_kernel) {
        // ?name= selects the kernel file; otherwise the kernel the current config.txt boots.
        char name[AUDIOX_UPDATE_VALUE_MAX];
        if (query && strstr(query, "name=")) {
            if (!queryKernelName(query, name, sizeof(name)) || !audioxUpdateValidKernelName(name)) {
                static const char bad[] = "invalid kernel name (expected kernel*.img)\n";
                return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
            }
        } else {
            currentBootKernelName(name, sizeof(name));
        }
        snprintf(kernel_path, sizeof(kernel_path), "%s/%s", HTTP_BOOT_DIR, name);
        file_path = kernel_path;
    }

    if (strcmp(method, "GET") == 0) {
        if (access(file_path, R_OK) < 0) {
            static const char err[] = "boot file unavailable\n";
            return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        }
        return server->sendFilePath(cfd, file_path, "application/octet-stream");
    }

    if (content_length == 0) {
        static const char bad[] = "missing body\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }
    if (content_length > HTTP_BOOT_UPLOAD_MAX) {
        static const char too_large[] = "payload too large\n";
        return server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
    }

    char temp_path[sizeof(kernel_path) + sizeof(".part")];
    int path_len = snprintf(temp_path, sizeof(temp_path), "%s.part", file_path);
    if (path_len <= 0 || (size_t)path_len >= sizeof(temp_path)) {
        static const char err[] = "boot file path too long\n";
        return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload open failed", errno);
    }

    int stream_rc = streamRequestBodyToFd(cfd, fd, initial_body, initial_body_len, content_length);
    if (stream_rc < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload failed", saved_errno);
    }
    if (stream_rc > 0) {
        close(fd);
        unlink(temp_path);
        static const char err[] = "incomplete upload\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }
    if (fsync(fd) < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload sync failed", saved_errno);
    }
    if (close(fd) < 0) {
        int saved_errno = errno;
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload close failed", saved_errno);
    }
    if (prepareBootUpdateRecovery() != RET_OK) {
        int saved_errno = errno;
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot backup failed", saved_errno);
    }
    if (rename(temp_path, file_path) < 0) {
        int saved_errno = errno;
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload finalize failed", saved_errno);
    }

    static const char ok[] = "{\"ok\":true}\n";
    return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleInitramUpload(HttpServer *server,
                               int cfd,
                               const char *initial_body,
                               size_t initial_body_len,
                               size_t content_length) {
    if (!server) {
        return -1;
    }

    if (content_length == 0) {
        static const char bad[] = "missing body\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    if (checkStageCapacity(server, cfd, content_length) < 0) {
        return 0;
    }

    if (ensureDir(HTTP_STAGE_DIR, 0755) != RET_OK) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging mkdir failed", errno);
    }

    int fd = open(HTTP_STAGE_INITRAMFS_PART_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging open failed", errno);
    }

    int stream_rc = streamRequestBodyToFd(cfd, fd, initial_body, initial_body_len, content_length);
    if (stream_rc < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging write failed", saved_errno);
    }
    if (stream_rc > 0) {
        close(fd);
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        static const char err[] = "incomplete upload\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    if (fsync(fd) < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging fsync failed", saved_errno);
    }

    close(fd);

    if (prepareBootUpdateRecovery() != RET_OK) {
        int saved_errno = errno;
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot backup failed", saved_errno);
    }

    if (rename(HTTP_STAGE_INITRAMFS_PART_PATH, HTTP_STAGE_INITRAMFS_PATH) < 0) {
        int saved_errno = errno;
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging finalize failed", saved_errno);
    }

    static const char ok[] = "ok staged rebooting\n";
    (void)server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
    server->rebootAfterResponse(RB_AUTOBOOT);
    return 0;
}

typedef AudioxUpdateEntry UpdateEntry;

static int readUpdateEntryText(int fd, const UpdateEntry &entry, char *buf, size_t buf_sz) {
    if (entry.size >= buf_sz) {
        errno = EFBIG;
        return -1;
    }
    if (audioxUpdateReadAt(fd, buf, (size_t)entry.size, entry.offset) < 0) {
        return -1;
    }
    buf[entry.size] = '\0';
    return 0;
}

// Reads the metadata (v1 archives default to kernel8.img with no board), rejects archives built
// for another board, and requires the archive's config.txt to boot the archive's kernel.
static const char *checkUpdateMetadata(int fd, const UpdateEntry entries[AUDIOX_UPDATE_KNOWN_TYPES], AudioxUpdateMetadata *meta) {
    const UpdateEntry &meta_entry = entries[AUDIOX_UPDATE_METADATA - 1u];
    if (meta_entry.size == 0) {
        memset(meta, 0, sizeof(*meta));
        snprintf(meta->kernel, sizeof(meta->kernel), "%s", AUDIOX_UPDATE_DEFAULT_KERNEL);
    } else {
        char text[AUDIOX_UPDATE_METADATA_MAX + 1];
        if (readUpdateEntryText(fd, meta_entry, text, sizeof(text)) < 0) {
            return "failed to read update metadata\n";
        }
        if (audioxUpdateParseMetadata(text, (size_t)meta_entry.size, meta) != NULL) {
            return "invalid update metadata\n";
        }
        const char *board = detectBoard();
        if (board[0] && strcmp(board, meta->board) != 0) {
            static char msg[128];
            snprintf(msg, sizeof(msg), "update archive is for %s but this device is %s\n", meta->board, board);
            return msg;
        }
    }

    const UpdateEntry &config_entry = entries[AUDIOX_UPDATE_CONFIG - 1u];
    char *config_text = (char *)malloc((size_t)config_entry.size + 1u);
    if (!config_text) {
        return "out of memory reading boot config\n";
    }
    if (readUpdateEntryText(fd, config_entry, config_text, (size_t)config_entry.size + 1u) < 0) {
        free(config_text);
        return "failed to read boot config entry\n";
    }
    char config_kernel[AUDIOX_UPDATE_VALUE_MAX];
    audioxConfigKernelName(config_text, (size_t)config_entry.size, config_kernel, sizeof(config_kernel));
    free(config_text);
    if (strcmp(config_kernel, meta->kernel) != 0) {
        return "update archive config.txt does not boot the archive kernel\n";
    }

    const UpdateEntry &cmdline_entry = entries[AUDIOX_UPDATE_CMDLINE - 1u];
    if (cmdline_entry.size != 0) {
        char cmdline[AUDIOX_UPDATE_CMDLINE_MAX + 1];
        if (readUpdateEntryText(fd, cmdline_entry, cmdline, sizeof(cmdline)) < 0) {
            return "failed to read cmdline entry\n";
        }
        if (audioxValidateCmdline(cmdline, (size_t)cmdline_entry.size) != NULL) {
            return "update archive cmdline.txt is invalid (must be one line containing rdinit=/init)\n";
        }
    }
    return NULL;
}

static int writeUpdateEntry(int archive_fd, const UpdateEntry &entry, const char *dest_path) {
    int out_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        return -1;
    }

    unsigned char buf[16384];
    uint64_t done = 0;
    while (done < entry.size) {
        size_t want = sizeof(buf);
        if (entry.size - done < want) {
            want = (size_t)(entry.size - done);
        }
        if (audioxUpdateReadAt(archive_fd, buf, want, entry.offset + (off_t)done) < 0 ||
            writeAllToFd(out_fd, buf, want) < 0) {
            int saved_errno = errno;
            close(out_fd);
            unlink(dest_path);
            errno = saved_errno;
            return -1;
        }
        done += want;
    }

    if (fsync(out_fd) < 0) {
        int saved_errno = errno;
        close(out_fd);
        unlink(dest_path);
        errno = saved_errno;
        return -1;
    }
    if (close(out_fd) < 0) {
        int saved_errno = errno;
        unlink(dest_path);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static void cleanupUpdateFiles(void) {
    unlink(HTTP_STAGE_UPDATE_PATH);
    unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
    unlink(HTTP_BOOT_CONFIG_FILE ".update");
    unlink(HTTP_BOOT_CMDLINE_FILE ".update");
    unlink(HTTP_BOOT_LOADER_FILE ".update");
    unlink(HTTP_BOOT_KERNEL_TEMP_FILE);
}

static int handleUpdateUpload(HttpServer *server,
                              int cfd,
                              const char *initial_body,
                              size_t initial_body_len,
                              size_t content_length) {
    if (content_length == 0) {
        static const char bad[] = "missing body\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    // The archive and its extracted runtime are both temporarily stored in staging.
    if (checkStageCapacity(server, cfd, content_length * 2u) < 0) {
        return 0;
    }
    if (ensureDir(HTTP_STAGE_DIR, 0755) != RET_OK) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging mkdir failed", errno);
    }

    cleanupUpdateFiles();
    int archive_fd = open(HTTP_STAGE_UPDATE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (archive_fd < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "update archive open failed", errno);
    }

    int stream_rc = streamRequestBodyToFd(cfd, archive_fd, initial_body, initial_body_len, content_length);
    if (stream_rc < 0) {
        int saved_errno = errno;
        close(archive_fd);
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "update archive write failed", saved_errno);
    }
    if (stream_rc > 0) {
        close(archive_fd);
        cleanupUpdateFiles();
        static const char err[] = "incomplete upload\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }
    if (fsync(archive_fd) < 0) {
        int saved_errno = errno;
        close(archive_fd);
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "update archive sync failed", saved_errno);
    }

    UpdateEntry entries[AUDIOX_UPDATE_KNOWN_TYPES];
    memset(entries, 0, sizeof(entries));
    const char *parse_error = audioxUpdateParseArchive(archive_fd, content_length, entries);
    if (parse_error) {
        close(archive_fd);
        cleanupUpdateFiles();
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", parse_error, strlen(parse_error));
    }
    AudioxUpdateMetadata meta;
    const char *meta_error = checkUpdateMetadata(archive_fd, entries, &meta);
    if (meta_error) {
        close(archive_fd);
        cleanupUpdateFiles();
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", meta_error, strlen(meta_error));
    }
    printf("[HTTP] update archive: board=%s kernel=%s version=%s kernel_version=%s\n",
           meta.board[0] ? meta.board : "unspecified", meta.kernel,
           meta.version[0] ? meta.version : "unspecified",
           meta.kernel_version[0] ? meta.kernel_version : "unspecified");
    if (mountBootPartition() < 0) {
        close(archive_fd);
        cleanupUpdateFiles();
        static const char err[] = "boot partition unavailable\n";
        return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    char kernel_path[sizeof(HTTP_BOOT_DIR) + AUDIOX_UPDATE_VALUE_MAX + 1];
    snprintf(kernel_path, sizeof(kernel_path), "%s/%s", HTTP_BOOT_DIR, meta.kernel);

    struct UpdateTarget {
        uint32_t type;
        const char *temp_path;
        const char *final_path;
    };
    // config.txt goes last: it selects the kernel, so it only switches once that kernel exists.
    // The optional cmdline entry is skipped when absent so the device keeps its current one.
    const UpdateTarget boot_targets[] = {
        {AUDIOX_UPDATE_KERNEL, HTTP_BOOT_KERNEL_TEMP_FILE, kernel_path},
        {AUDIOX_UPDATE_BOOTLOADER, HTTP_BOOT_LOADER_FILE ".update", HTTP_BOOT_LOADER_FILE},
        {AUDIOX_UPDATE_CMDLINE, HTTP_BOOT_CMDLINE_FILE ".update", HTTP_BOOT_CMDLINE_FILE},
        {AUDIOX_UPDATE_CONFIG, HTTP_BOOT_CONFIG_FILE ".update", HTTP_BOOT_CONFIG_FILE},
    };

    if (writeUpdateEntry(archive_fd, entries[AUDIOX_UPDATE_PROGRAM - 1u], HTTP_STAGE_INITRAMFS_PART_PATH) < 0) {
        int saved_errno = errno;
        close(archive_fd);
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "runtime extract failed", saved_errno);
    }
    for (const UpdateTarget &target : boot_targets) {
        if (entries[target.type - 1u].size == 0) {
            continue;
        }
        if (writeUpdateEntry(archive_fd, entries[target.type - 1u], target.temp_path) < 0) {
            int saved_errno = errno;
            close(archive_fd);
            cleanupUpdateFiles();
            return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot file extract failed", saved_errno);
        }
    }
    close(archive_fd);

    if (prepareBootUpdateRecovery() != RET_OK) {
        int saved_errno = errno;
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot backup failed", saved_errno);
    }

    for (const UpdateTarget &target : boot_targets) {
        if (entries[target.type - 1u].size == 0) {
            continue;
        }
        if (rename(target.temp_path, target.final_path) < 0) {
            int saved_errno = errno;
            cleanupUpdateFiles();
            return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot file install failed", saved_errno);
        }
    }
    sync();

    if (rename(HTTP_STAGE_INITRAMFS_PART_PATH, HTTP_STAGE_INITRAMFS_PATH) < 0) {
        int saved_errno = errno;
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "runtime staging failed", saved_errno);
    }
    unlink(HTTP_STAGE_UPDATE_PATH);

    static const char ok[] = "ok update installed rebooting\n";
    (void)server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
    server->rebootAfterResponse(RB_AUTOBOOT);
    return 0;
}

static int safeApiSuffix(const char *suffix) {
    if (!suffix) {
        return 0;
    }
    if (!suffix[0]) {
        return 1;
    }
    if (strstr(suffix, "..") != NULL) {
        return 0;
    }
    if (strchr(suffix, '\\') != NULL) {
        return 0;
    }
    return 1;
}

static int ensureParentDirs(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    char tmp[512];
    size_t n = strnlen(path, sizeof(tmp) - 1);
    if (n == 0 || n >= sizeof(tmp) - 1) {
        return -1;
    }
    memcpy(tmp, path, n);
    tmp[n] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }

    return 0;
}

static int isSfxUploadSuffix(const char *suffix) {
    return (suffix && strncmp(suffix, "sfx/", 4) == 0 && suffix[4] != '\0') ? 1 : 0;
}

static int buildSfxFinalWavPath(const char *inputPath, char *out, size_t outSz) {
    if (!inputPath || !out || outSz == 0) {
        return -1;
    }

    size_t n = strnlen(inputPath, outSz - 1);
    if (n == 0 || n >= outSz - 1) {
        return -1;
    }
    memcpy(out, inputPath, n);
    out[n] = '\0';

    char *slash = strrchr(out, '/');
    char *leaf = slash ? slash + 1 : out;
    if (!leaf[0]) {
        return -1;
    }

    char *dot = strrchr(leaf, '.');
    if (dot) {
        *dot = '\0';
    }

    size_t stemLen = strlen(out);
    if (stemLen + 4 >= outSz) {
        return -1;
    }
    memcpy(out + stemLen, ".wav", 5);
    return 0;
}

static pid_t startFfmpegTranscode(int inputFd, const char *outputPath) {
    const char *ffmpeg_path = getFfmpegPath();
    if (!ffmpeg_path) {
        errno = ENOENT;
        return -1;
    }
    char inputPath[64];
    snprintf(inputPath, sizeof(inputPath), "/proc/self/fd/%d", inputFd);
    const pid_t parent = getpid();

    pid_t pid = fork();
    if (pid == 0) {
        const struct rlimit fileLimit{HTTP_ROOTFS_UPLOAD_MAX, HTTP_ROOTFS_UPLOAD_MAX};
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0 || getppid() != parent ||
            fcntl(inputFd, F_SETFD, 0) < 0 || setrlimit(RLIMIT_FSIZE, &fileLimit) < 0) _exit(127);
        int devNull = open("/dev/null", O_RDWR);
        if (devNull >= 0) {
            (void)dup2(devNull, STDIN_FILENO);
            (void)dup2(devNull, STDOUT_FILENO);
            (void)dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) {
                close(devNull);
            }
        }

        execl(ffmpeg_path,
              "ffmpeg",
              "-hide_banner",
              "-loglevel", "error",
              "-nostdin",
              "-xerror",
              "-y",
              "-protocol_whitelist", "file",
              "-i", inputPath,
              "-vn",
              "-map_metadata", "-1",
              "-fflags", "+bitexact",
              "-acodec", "pcm_s16le",
              "-ar", "48000",
              "-ac", "2",
              "-f", "wav",
              outputPath,
              (char *)NULL);
        
        _exit(127);
    }
    return pid;
}

static int startSoundImport(HttpServer *server, int cfd, const char *uploadedPath) {
    if (!exchange || exchange->upload < 0 || !server->transport) return -1;
    auto &job = server->transport->sound;
    if (job.request || job.pid >= 0) {
        static const char busy[] = "another sound import is in progress; try again shortly\n";
        return server->sendResponse(cfd, "503 Service Unavailable", "text/plain", busy, sizeof(busy) - 1);
    }
    char wavPath[512];
    if (buildSfxFinalWavPath(uploadedPath, wavPath, sizeof(wavPath)) < 0) {
        static const char bad[] = "bad sfx path\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain", bad, sizeof(bad) - 1);
    }
    std::string temporary = HTTP_STAGE_DIR "/.audiox-import-XXXXXX";
    const int fd = mkostemp(temporary.data(), O_CLOEXEC);
    if (fd < 0) return sendErrnoResponse(server, cfd, "500 Internal Server Error", "stage converted sound", errno);
    close(fd);
    const pid_t pid = startFfmpegTranscode(exchange->upload, temporary.c_str());
    if (pid < 0) {
        const int error = errno;
        unlink(temporary.c_str());
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "start ffmpeg", error);
    }
    job.pid = pid;
    job.deadline = httpMonotonicMs() + 120000;
    job.temporary = std::move(temporary);
    job.destination = wavPath;
    return 0;
}

static bool validConvertedSound(int fd) {
    // FFmpeg's bit-exact stereo PCM muxer writes a 44-byte WAV header.
    unsigned char header[44];
    static constexpr unsigned char format[] = {
        16, 0, 0, 0, 1, 0, 2, 0, 0x80, 0xbb, 0, 0, 0, 0xee, 2, 0, 4, 0, 16, 0
    };
    if (audioxUpdateReadAt(fd, header, sizeof(header), 0) < 0 ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVEfmt ", 8) ||
        memcmp(header + 16, format, sizeof(format)) || memcmp(header + 36, "data", 4)) return false;
    const uint32_t bytes = header[40] | (uint32_t(header[41]) << 8) |
        (uint32_t(header[42]) << 16) | (uint32_t(header[43]) << 24);
    return bytes >= 4 && bytes % 4 == 0 && lseek(fd, 0, SEEK_END) == off_t(sizeof(header)) + bytes;
}

static void pollSoundImport(HttpServer *server, uint64_t now) {
    auto &job = server->transport->sound;
    if (!job.request) return;
    if (now >= job.deadline || job.request->disconnected.load()) {
        job.timedOut = now >= job.deadline;
        (void)kill(job.pid, SIGKILL);
    }
    int status = 0;
    const pid_t result = waitpid(job.pid, &status, WNOHANG);
    if (result == 0 || (result < 0 && errno == EINTR)) return;

    exchange = job.request.get();
    if (!exchange->disconnected.load()) {
        int fd = -1;
        if (job.timedOut) {
            static const char error[] = "sound conversion timed out\n";
            server->sendResponse(-1, "504 Gateway Timeout", "text/plain", error, sizeof(error) - 1);
        } else if (result < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
            static const char error[] = "ffmpeg conversion failed\n";
            server->sendResponse(-1, "415 Unsupported Media Type", "text/plain", error, sizeof(error) - 1);
        } else if ((fd = open(job.temporary.c_str(), O_RDWR | O_CLOEXEC)) < 0 ||
                   fchmod(fd, 0644) < 0 || fsync(fd) < 0) {
            sendErrnoResponse(server, -1, "500 Internal Server Error", "sync converted sound", errno);
        } else if (!validConvertedSound(fd)) {
            static const char error[] = "sound conversion produced no usable PCM audio\n";
            server->sendResponse(-1, "415 Unsupported Media Type", "text/plain", error, sizeof(error) - 1);
        } else if (!exchange->disconnected.load()) {
            if (rename(job.temporary.c_str(), job.destination.c_str()) < 0) {
                sendErrnoResponse(server, -1, "500 Internal Server Error", "publish converted sound", errno);
            } else {
                const std::string dir = job.destination.substr(0, job.destination.rfind('/'));
                const int dirFd = open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
                if (dirFd < 0 || fsync(dirFd) < 0) {
                    printf("[HTTP] [WARN] sound published but directory sync failed: %s\n", strerror(errno));
                }
                if (dirFd >= 0) close(dirFd);
                if (server->app && server->app->audio && server->app->audio->reloadSfxBank() == RET_ERR) {
                    printf("[HTTP] [WARN] sound published but soundboard reload failed\n");
                    static const char error[] = "sound saved, but the soundboard could not reload\n";
                    server->sendResponse(-1, "500 Internal Server Error", "text/plain", error, sizeof(error) - 1);
                } else {
                    const std::string ok = "ok converted " + job.destination.substr(job.destination.rfind('/') + 1) + "\n";
                    server->sendResponse(-1, "200 OK", "text/plain; charset=utf-8", ok.data(), ok.size());
                }
            }
        }
        if (fd >= 0) close(fd);
    }
    unlink(job.temporary.c_str());
    exchange->complete.store(true);
    exchange = nullptr;
    job = {};
}

static int handleRootfsUpload(HttpServer *server,
                              int cfd,
                              const char *path,
                              const char *initial_body,
                              size_t initial_body_len,
                              size_t content_length) {
    if (!server || !path || strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX)) != 0) {
        return -1;
    }

    const char *suffix = path + strlen(HTTP_ROOTFS_PREFIX);
    if (!safeApiSuffix(suffix) || !suffix[0] || suffix[strlen(suffix) - 1] == '/') {
        static const char bad[] = "bad file path\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }
    if (!audiox::config_storage::rootfsWritable(ROOT_MOUNT_POINT, suffix)) {
        static const char error[] = "configuration schema is unsupported, unreadable, or reserved\n";
        printf("[CONFIG] [WARN] refusing configuration upload: %s\n", suffix);
        return server->sendResponse(cfd, "409 Conflict", "text/plain", error, sizeof(error) - 1);
    }

    if (content_length == 0) {
        static const char bad[] = "missing body\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    if (content_length > HTTP_ROOTFS_UPLOAD_MAX) {
        static const char too_large[] = "payload too large\n";
        return server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
    }

    char fs_path[512];
    int pn = snprintf(fs_path, sizeof(fs_path), "%s%s", ROOT_MOUNT_POINT "/",
                      audiox::config_storage::rootfsPath(suffix).c_str());
    if (pn <= 0 || (size_t)pn >= sizeof(fs_path)) {
        static const char bad[] = "path too long\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    if (ensureParentDirs(fs_path) < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "mkdir failed", errno);
    }
    if (isSfxUploadSuffix(suffix)) return startSoundImport(server, cfd, fs_path);

    audiox::AtomicConfigFile output(fs_path);
    if (!output.get()) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "open failed", errno);
    }
    int fd = fileno(output.get());

    int stream_rc = streamRequestBodyToFd(cfd, fd, initial_body, initial_body_len, content_length);
    if (stream_rc < 0) {
        int saved_errno = errno;
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "write failed", saved_errno);
    }
    if (stream_rc > 0) {
        static const char err[] = "incomplete upload\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    if (output.commit() < 0) {
        int saved_errno = errno;
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "fsync failed", saved_errno);
    }

    // Auto-set executable permissions for common binary paths
    const char *leaf = strrchr(suffix, '/');
    leaf = leaf ? (leaf + 1) : suffix;
    if (strncmp(leaf, "ffmpeg", 6) == 0 ||
        strstr(suffix, "/bin/") != NULL ||
        strstr(suffix, "/sbin/") != NULL) {
        chmod(fs_path, 0755);
    }

    static const char ok[] = "ok\n";
    return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int checkRestoreStorage(HttpServer *server, int cfd, size_t contentLength) {
    if (contentLength < 20 || contentLength > audiox::backup::maxArchiveSize) {
        static const char message[] = "backup must be between 20 bytes and 4 GiB\n";
        return server->sendResponse(cfd, "413 Payload Too Large", "text/plain", message, sizeof(message) - 1) == 0 ? 1 : -1;
    }
    struct statvfs storage;
    if (statvfs(ROOT_MOUNT_POINT, &storage) < 0) {
        const int error = errno;
        printf("[CONFIG] [ERR] restore storage check failed: %s\n", strerror(error));
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "check restore storage", error) == 0 ? 1 : -1;
    }
    const uint64_t freeBytes = (uint64_t)storage.f_bavail * storage.f_frsize;
    const uint64_t required = (uint64_t)contentLength * 2 + 16 * 1024 * 1024;
    if (freeBytes < required) {
        char message[256];
        int n = snprintf(message, sizeof(message),
                         "insufficient storage for restore: need %llu bytes, have %llu bytes free; no settings changed\n",
                         (unsigned long long)required, (unsigned long long)freeBytes);
        printf("[CONFIG] [WARN] %s", message);
        return server->sendResponse(cfd, "507 Insufficient Storage", "text/plain", message, (size_t)n) == 0 ? 1 : -1;
    }
    return 0;
}

static int handleRestoreCheck(HttpServer *server, int cfd, const char *method, const char *query) {
    if (strcmp(method, "GET") != 0) {
        static const char message[] = "method not allowed\n";
        return server->sendResponse(cfd, "405 Method Not Allowed", "text/plain", message, sizeof(message) - 1);
    }
    char *end = nullptr;
    const char *size = query && strncmp(query, "size=", 5) == 0 ? query + 5 : "";
    errno = 0;
    const unsigned long long bytes = strtoull(size, &end, 10);
    if (!size[0] || size[0] < '0' || size[0] > '9' || *end || errno == ERANGE ||
        bytes > audiox::backup::maxArchiveSize) {
        static const char message[] = "invalid backup size\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain", message, sizeof(message) - 1);
    }
    const int rc = checkRestoreStorage(server, cfd, (size_t)bytes);
    if (rc != 0) return rc < 0 ? -1 : 0;
    static const char message[] = "restore storage available\n";
    return server->sendResponse(cfd, "200 OK", "text/plain", message, sizeof(message) - 1);
}

static int handleConfigBackup(HttpServer *server, int cfd, const char *method,
                              const char *path, const char *initial, size_t initialLen,
                              size_t contentLength) {
    bool restoring = strcmp(path, "/api/config/restore") == 0;
    if (strcmp(method, restoring ? "PUT" : "GET")) {
        static const char message[] = "method not allowed\n";
        return server->sendResponse(cfd, "405 Method Not Allowed", "text/plain", message, sizeof(message) - 1);
    }
    const bool staged = restoring && exchange && exchange->upload >= 0;
    if (restoring && !staged) {
        const int checked = checkRestoreStorage(server, cfd, contentLength);
        if (checked != 0) return checked < 0 ? -1 : 0;
    }
    char temp[] = ROOT_MOUNT_POINT "/.backup-transfer-XXXXXX";
    int fd = staged ? fcntl(exchange->upload, F_DUPFD_CLOEXEC, 0) : mkostemp(temp, O_CLOEXEC);
    if (fd < 0) return sendErrnoResponse(server, cfd, "500 Internal Server Error", "create backup transfer", errno);
    if (!staged && unlink(temp) < 0) {
        int saved = errno;
        close(fd);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "unlink backup transfer", saved);
    }
    char error[256] = {};
    int rc;
    if (restoring) {
        rc = staged ? 0 : streamRequestBodyToFd(cfd, fd, initial, initialLen, contentLength);
        if (rc) {
            int saved = errno;
            close(fd);
            printf("[CONFIG] [ERR] backup upload failed: %s\n", rc < 0 ? strerror(saved) : "incomplete upload");
            if (rc < 0) return sendErrnoResponse(server, cfd, "500 Internal Server Error", "receive backup", saved);
            static const char message[] = "incomplete backup upload; no settings changed\n";
            return server->sendResponse(cfd, "400 Bad Request", "text/plain", message, sizeof(message) - 1);
        }
        rc = audiox::backup::stage(ROOT_MOUNT_POINT, fd, contentLength, error, sizeof(error));
    } else {
        rc = audiox::backup::create(ROOT_MOUNT_POINT, fd, error, sizeof(error));
    }
    if (rc < 0) {
        close(fd);
        printf("[CONFIG] [ERR] %s failed: %s\n", restoring ? "restore" : "backup", error);
        return server->sendResponse(cfd, restoring ? "400 Bad Request" : "500 Internal Server Error",
                                    "text/plain; charset=utf-8", error, strlen(error));
    }
    if (restoring) {
        close(fd);
        static const char message[] = "Backup validated and staged; rebooting to restore settings and sounds.\n";
        rc = server->sendResponse(cfd, "202 Accepted", "text/plain; charset=utf-8", message, sizeof(message) - 1);
        server->rebootAfterResponse(RB_AUTOBOOT);
        return rc;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        int saved = errno;
        close(fd);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "size backup", saved);
    }
    rc = server->sendResponse(cfd, "200 OK", "application/octet-stream", nullptr, (size_t)size, "audiox-backup.axb");
    if (exchange && rc == 0) {
        exchange->download = fd;
        exchange->downloadSize = size;
        return rc;
    }
    char buf[65536];
    off_t pos = 0;
    while (rc == 0 && pos < size) {
        audioxWatchdogHeartbeat();
        size_t want = (uint64_t)(size - pos) > sizeof(buf) ? sizeof(buf) : (size_t)(size - pos);
        if (audioxUpdateReadAt(fd, buf, want, pos) < 0) {
            rc = -1;
            break;
        }
        size_t sent = 0;
        while (sent < want) {
            audioxWatchdogHeartbeat();
            ssize_t n = send(cfd, buf + sent, want - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { rc = -1; break; }
            sent += (size_t)n;
        }
        pos += (off_t)want;
    }
    if (rc < 0) printf("[CONFIG] [ERR] backup download interrupted: %s\n", strerror(errno));
    close(fd);
    return rc;
}

static ssize_t findHeaderEnd(const char *buf, size_t len) {
    if (!buf || len < 4) {
        return -1;
    }

    for (size_t i = 0; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (ssize_t)(i + 4);
        }
    }
    for (size_t i = 0; i + 1 < len; ++i) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            return (ssize_t)(i + 2);
        }
    }
    return -1;
}

static bool headerValue(const char *start, size_t length, char *out, size_t capacity) {
    while (length && (*start == ' ' || *start == '\t')) {
        ++start;
        --length;
    }
    while (length && (start[length - 1] == ' ' || start[length - 1] == '\t')) --length;
    if (!length || length >= capacity) return false;
    for (size_t i = 0; i < length; ++i) {
        const unsigned char c = static_cast<unsigned char>(start[i]);
        if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
    }
    memcpy(out, start, length);
    out[length] = '\0';
    return true;
}

static bool parseRequestHeaders(const char *buf, size_t headerLen, HttpRequestHeaders *out) {
    if (!buf || !out || !headerLen || headerLen > 16384) return false;
    *out = HttpRequestHeaders{};
    snprintf(out->host, sizeof(out->host), "%s", AUDIOX_USB_NET_SERVER_IP);

    const char *end = buf + headerLen;
    const char *line = static_cast<const char *>(memchr(buf, '\n', headerLen));
    if (!line) return false;
    ++line;
    while (line < end) {
        const char *next = static_cast<const char *>(memchr(line, '\n', end - line));
        const char *stop = next ? next : end;
        if (stop > line && stop[-1] == '\r') --stop;
        if (stop == line) break;
        const char *colon = static_cast<const char *>(memchr(line, ':', stop - line));
        if (!colon || colon == line) return false;
        for (const char *p = colon + 1; p < stop; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if ((c < 0x20 && c != '\t') || c == 0x7f) return false;
        }
        for (const char *p = line; p < colon; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '-')) return false;
        }
        const char *value = colon + 1;
        const size_t valueLength = static_cast<size_t>(stop - value);
        const size_t nameLength = static_cast<size_t>(colon - line);
        if (nameLength == 17 && !strncasecmp(line, "Transfer-Encoding", nameLength))
            return false;
        if (nameLength == 14 && !strncasecmp(line, "Content-Length", nameLength)) {
            if (out->hasContentLength) return false;
            while (value < stop && (*value == ' ' || *value == '\t')) ++value;
            const char *trimmedEnd = stop;
            while (trimmedEnd > value && (trimmedEnd[-1] == ' ' || trimmedEnd[-1] == '\t')) --trimmedEnd;
            if (value == trimmedEnd) return false;
            size_t parsed = 0;
            for (const char *p = value; p < trimmedEnd; ++p) {
                if (*p < '0' || *p > '9' || parsed > (SIZE_MAX - (*p - '0')) / 10) return false;
                parsed = parsed * 10 + static_cast<size_t>(*p - '0');
            }
            if (parsed > static_cast<size_t>(SSIZE_MAX)) return false;
            out->contentLength = parsed;
            out->hasContentLength = true;
        } else {
            char *destination = nullptr;
            size_t capacity = 0;
            bool *seen = nullptr;
            if (nameLength == 4 && !strncasecmp(line, "Host", 4)) {
                destination = out->host; capacity = sizeof(out->host); seen = &out->hasHost;
            } else if (nameLength == 7 && !strncasecmp(line, "Upgrade", 7)) {
                destination = out->upgrade; capacity = sizeof(out->upgrade); seen = &out->hasUpgrade;
            } else if (nameLength == 10 && !strncasecmp(line, "Connection", 10)) {
                destination = out->connection; capacity = sizeof(out->connection); seen = &out->hasConnection;
            } else if (nameLength == 17 && !strncasecmp(line, "Sec-WebSocket-Key", 17)) {
                destination = out->websocketKey; capacity = sizeof(out->websocketKey); seen = &out->hasWebSocketKey;
            } else if (nameLength == 21 && !strncasecmp(line, "Sec-WebSocket-Version", 21)) {
                destination = out->websocketVersion; capacity = sizeof(out->websocketVersion); seen = &out->hasWebSocketVersion;
            } else if (nameLength == 22 && !strncasecmp(line, "Sec-WebSocket-Protocol", 22)) {
                destination = out->websocketProtocol; capacity = sizeof(out->websocketProtocol); seen = &out->hasWebSocketProtocol;
            } else if (nameLength == 6 && !strncasecmp(line, "Origin", 6)) {
                destination = out->origin; capacity = sizeof(out->origin); seen = &out->hasOrigin;
            } else if (nameLength == 14 && !strncasecmp(line, "Sec-Fetch-Site", 14)) {
                destination = out->fetchSite; capacity = sizeof(out->fetchSite); seen = &out->hasFetchSite;
            }
            if (destination) {
                if (seen && *seen) return false;
                if (!headerValue(value, valueLength, destination, capacity)) return false;
                if (seen) *seen = true;
                if (nameLength == 4 && !strncasecmp(line, "Host", 4)) {
                    for (const char *p = destination; *p; ++p) {
                        if (!(isalnum(static_cast<unsigned char>(*p)) || *p == '.' ||
                              *p == '-' || *p == ':' || *p == '[' || *p == ']')) return false;
                    }
                }
            }
        }
        if (!next) break;
        line = next + 1;
    }
    return true;
}

static bool parseRequestLine(const char *request, size_t headerLen,
                             char *method, size_t methodCapacity,
                             char *target, size_t targetCapacity, bool *http11) {
    const char *end = static_cast<const char *>(memchr(request, '\n', headerLen));
    if (!end) return false;
    if (end > request && end[-1] == '\r') --end;
    const char *firstSpace = static_cast<const char *>(memchr(request, ' ', end - request));
    if (!firstSpace || firstSpace == request) return false;
    const char *secondSpace = static_cast<const char *>(memchr(firstSpace + 1, ' ', end - firstSpace - 1));
    if (!secondSpace || secondSpace == firstSpace + 1 || memchr(secondSpace + 1, ' ', end - secondSpace - 1))
        return false;
    const size_t methodLength = static_cast<size_t>(firstSpace - request);
    const size_t targetLength = static_cast<size_t>(secondSpace - firstSpace - 1);
    if (methodLength >= methodCapacity || targetLength >= targetCapacity) return false;
    for (const char *p = request; p < firstSpace; ++p)
        if (*p < 'A' || *p > 'Z') return false;
    memcpy(method, request, methodLength);
    method[methodLength] = '\0';
    memcpy(target, firstSpace + 1, targetLength);
    target[targetLength] = '\0';
    if (target[0] != '/' || strchr(target, '#') || strchr(target, '\\')) return false;
    for (size_t i = 0; i < targetLength; ++i)
        if (static_cast<unsigned char>(target[i]) < 0x21 || target[i] == 0x7f) return false;
    const char *version = secondSpace + 1;
    const size_t versionLength = static_cast<size_t>(end - version);
    if (versionLength == 8 && !memcmp(version, "HTTP/1.1", 8)) {
        *http11 = true;
        return true;
    }
    if (versionLength == 8 && !memcmp(version, "HTTP/1.0", 8)) {
        *http11 = false;
        return true;
    }
    return false;
}

static int setNonBlocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0)
    return -1;
  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
    return -1;
  }
  return 0;
}

static void loadHtmlBody(HttpServer *server, const char *html_path) {
  if (!server) {
    return;
  }

  static const char fallback[] =
      "<!doctype html><html><head><meta "
      "charset=\"utf-8\"><title>audiox</title></head>"
      "<body><h1>audiox</h1><p>Web UI placeholder</p></body></html>\n";

  memset(server->html_path, 0, sizeof(server->html_path));
  const char *use_path =
      (html_path && html_path[0]) ? html_path : HTTP_DEFAULT_HTML_PATH;
  size_t p_len = strnlen(use_path, sizeof(server->html_path) - 1);
  memcpy(server->html_path, use_path, p_len);
  server->html_path[p_len] = '\0';

  size_t fb_len = sizeof(fallback) - 1;
  memcpy(server->body, fallback, fb_len);
  server->body_len = fb_len;

  int fd = open(server->html_path, O_RDONLY);
  if (fd < 0) {
    return;
  }

  ssize_t n = read(fd, server->body, sizeof(server->body));
  close(fd);
  if (n > 0) {
    server->body_len = (size_t)n;
  }
}

static bool allowedRequestHost(const char *host) {
    if (!host) return false;
    char candidate[256];
    snprintf(candidate, sizeof(candidate), "%s", host);
    char *port = strrchr(candidate, ':');
    if (port && !strcmp(port, ":80")) *port = '\0';
    return !strcasecmp(candidate, AUDIOX_USB_NET_SERVER_IP) ||
        !strcasecmp(candidate, "audiox.local") ||
        !strcasecmp(candidate, "audiox") ||
        !strcasecmp(candidate, "audiox.lan") ||
        !strcasecmp(candidate, "audiox.wklab.net") ||
        !strcasecmp(candidate, "localhost");
}

static void handleClient(HttpServer *server, int cfd) {
    char req[HTTP_REQ_MAX + 1];
    size_t have = 0;
    ssize_t header_end = -1;

    while (have + 1 < sizeof(req)) {
        ssize_t n = readRequest(cfd, req + have, sizeof(req) - have - 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        have += (size_t)n;
        req[have] = '\0';

        header_end = findHeaderEnd(req, have);
        if (header_end >= 0) {
            break;
        }
    }

    if (header_end < 0) {
        static const char bad[] = "bad request\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }
    if (static_cast<size_t>(header_end) > HTTP_HEADER_MAX) {
        static const char too_large[] = "request headers too large\n";
        (void)server->sendResponse(cfd, "431 Request Header Fields Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
        return;
    }

    char method[8];
    char path[2048];
    bool http11 = false;
    if (!parseRequestLine(req, static_cast<size_t>(header_end),
                          method, sizeof(method), path, sizeof(path), &http11)) {
        static const char bad[] = "bad request line\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }

    HttpRequestHeaders headers{};
    if (!parseRequestHeaders(req, static_cast<size_t>(header_end), &headers) ||
        (http11 && !headers.hasHost)) {
        static const char bad[] = "invalid request headers\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }
    if (headers.hasHost && !allowedRequestHost(headers.host)) {
        static const char error[] = "unrecognized Host header\n";
        (void)server->sendResponse(cfd, "421 Misdirected Request",
            "text/plain; charset=utf-8", error, sizeof(error) - 1);
        return;
    }
    snprintf(server->requestHost, sizeof(server->requestHost), "%s",
             allowedRequestHost(headers.host) ? headers.host : AUDIOX_USB_NET_SERVER_IP);

    char *query = strchr(path, '?');
    if (query) {
        *query++ = '\0';
    }

    const ssize_t content_length = static_cast<ssize_t>(headers.contentLength);

    size_t body_off = (size_t)header_end;
    size_t initial_body_len = have > body_off ? have - body_off : 0;

    if (strcmp(path, "/ws") == 0 || strcmp(path, "/ws/desktop") == 0) {
        if (strcmp(method, "GET") || query || initial_body_len ||
            !addWebSocketClient(server, cfd, headers, !strcmp(path, "/ws/desktop"))) {
            static const char error[] = "invalid or unavailable websocket upgrade\n";
            (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", error, sizeof(error) - 1);
        }
        return;
    }
    struct ApiResponseScope {
        HttpServer *server;
        bool previous;
        ~ApiResponseScope() { server->apiRequest = previous; }
    } apiResponseScope{server, server->apiRequest};
    if (!strncmp(path, "/api/", 5) || !strcmp(path, "/api")) server->apiRequest = true;
    const bool safeMethod = !strcmp(method, "GET") || !strcmp(method, "HEAD") ||
        !strcmp(method, "OPTIONS");
    if (!safeMethod && ((headers.hasOrigin && !sameOriginHost(headers.origin, server->requestHost)) ||
                        !strcasecmp(headers.fetchSite, "cross-site"))) {
        static const char error[] = "{\"ok\":false,\"error\":\"cross-origin request rejected\"}\n";
        (void)server->sendResponse(cfd, "403 Forbidden", "application/json; charset=utf-8",
                                   error, sizeof(error) - 1);
        return;
    }
    if (initial_body_len > headers.contentLength) {
        static const char bad[] = "unexpected request bytes\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }
    if (strcmp(path, "/api/plugins/install") == 0) {
        if (strcmp(method, "PUT") || content_length <= 0 || content_length > 12 * 1024 * 1024) {
            const char *status = strcmp(method, "PUT") ? "405 Method Not Allowed" : "413 Payload Too Large";
            const char *error = "expected PUT with a nonempty AXM package up to 12 MiB\n";
            (void)server->sendResponse(cfd, status, "text/plain; charset=utf-8", error, strlen(error));
            return;
        }
        if (setSocketTimeouts(cfd, 10000, 10000) < 0) {
            (void)sendErrnoResponse(server, cfd, "500 Internal Server Error", "set plugin transfer timeout", errno);
            return;
        }
        std::string package(req + body_off, std::min(initial_body_len, static_cast<size_t>(content_length)));
        package.reserve(static_cast<size_t>(content_length));
        char chunk[8192];
        while (package.size() < static_cast<size_t>(content_length)) {
            ssize_t got = readRequest(cfd, chunk, std::min(sizeof(chunk), static_cast<size_t>(content_length) - package.size()));
            if (got < 0 && errno == EINTR) continue;
            if (got <= 0) {
                const char *error = "incomplete plugin upload; no package installed\n";
                (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", error, strlen(error));
                return;
            }
            package.append(chunk, static_cast<size_t>(got));
            audioxWatchdogHeartbeat();
        }
        (void)handleApiRequest(server, cfd, method, path, package.data(), package.size(), query);
        return;
    }

    if (strcmp(path, "/api/config/restore/check") == 0) {
        (void)handleRestoreCheck(server, cfd, method, query);
        return;
    }

    if (strcmp(path, "/api/config/backup") == 0 || strcmp(path, "/api/config/restore") == 0) {
        if (setSocketTimeouts(cfd, 10000, 10000) < 0) {
            (void)sendErrnoResponse(server, cfd, "500 Internal Server Error", "set backup transfer timeout", errno);
            return;
        }
        (void)handleConfigBackup(server, cfd, method, path, req + body_off, initial_body_len, (size_t)content_length);
        return;
    }

    if (strcmp(path, HTTP_BOOT_LOADER_PATH) == 0 ||
        strcmp(path, HTTP_BOOT_KERNEL_PATH) == 0) {
        if (strcmp(method, "PUT") == 0 &&
            setSocketTimeouts(cfd, 10000, 5000) < 0) {
            printf("[INIT] [WARN] failed to set boot upload socket timeout: %s\n", strerror(errno));
        }
        (void)handleBootFile(server,
                             cfd,
                             method,
                             path,
                             query,
                             (size_t)content_length,
                             req + body_off,
                             initial_body_len);
        return;
    }

    if (strcmp(path, HTTP_UPDATE_PATH) == 0) {
        if (strcmp(method, "PUT") != 0) {
            static const char mna[] = "method not allowed\n";
            (void)server->sendResponse(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
            return;
        }
        if ((size_t)content_length > HTTP_UPDATE_MAX) {
            static const char too_large[] = "payload too large\n";
            (void)server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
            return;
        }
        if (setSocketTimeouts(cfd, HTTP_INITRAM_RCV_TIMEOUT_SEC * 1000, 5000) < 0) {
            printf("[INIT] [WARN] failed to set update socket timeout: %s\n", strerror(errno));
        }
        (void)handleUpdateUpload(server,
                                 cfd,
                                 req + body_off,
                                 initial_body_len,
                                 (size_t)content_length);
        return;
    }

    if (strcmp(path, HTTP_INITRAM_PATH) == 0) {
        if (strcmp(method, "PUT") != 0) {
            static const char mna[] = "method not allowed\n";
            (void)server->sendResponse(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
            return;
        }
        if ((size_t)content_length > HTTP_INITRAM_MAX) {
            static const char too_large[] = "payload too large\n";
            (void)server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
            return;
        }
        if (setSocketTimeouts(cfd, HTTP_INITRAM_RCV_TIMEOUT_SEC * 1000, 5000) < 0) {
            printf("[INIT] [WARN] failed to extend upload socket timeout: %s\n", strerror(errno));
        }
        (void)handleInitramUpload(server,
                                  cfd,
                                  req + body_off,
                                  initial_body_len,
                                  (size_t)content_length);
        return;
    }

    if (strcmp(method, "PUT") == 0 &&
        strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX)) == 0) {
        if (setSocketTimeouts(cfd, 10000, 5000) < 0) {
            printf("[INIT] [WARN] failed to set rootfs upload socket timeout: %s\n", strerror(errno));
        }
        (void)handleRootfsUpload(server,
                                 cfd,
                                 path,
                                 req + body_off,
                                 initial_body_len,
                                 (size_t)content_length);
        return;
    }

    if ((size_t)content_length > HTTP_BODY_MAX) {
        static const char too_large[] = "payload too large\n";
        (void)server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
        return;
    }

    while ((have - body_off) < (size_t)content_length && have + 1 < sizeof(req)) {
        ssize_t n = readRequest(cfd, req + have, sizeof(req) - have - 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        have += (size_t)n;
        req[have] = '\0';
    }

    if ((have - body_off) < (size_t)content_length) {
        static const char bad[] = "incomplete body\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }

    const char *body = req + body_off;
    size_t body_len = (size_t)content_length;

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        if (server->sendFilePath(cfd, server->html_path, "text/html; charset=utf-8") < 0) {
            (void)server->sendResponse(cfd, "200 OK", "text/html; charset=utf-8", server->body, server->body_len);
        }
        return;
    }

    if (strcmp(method, "GET") == 0 && path[0] == '/' && strncmp(path, HTTP_API_PREFIX, strlen(HTTP_API_PREFIX)) != 0) {
        if (server->sendStaticFromWebRoot(cfd, path) == 0) {
            return;
        }
    }

    if (strncmp(path, HTTP_API_PREFIX, strlen(HTTP_API_PREFIX)) == 0) {
        if (handleApiRequest(server, cfd, method, path, body, body_len, query) == 0) {
            return;
        }
        static const char err[] = "{\"ok\":false,\"error\":\"api handler failed\"}\n";
        (void)server->sendResponse(cfd, "500 Internal Server Error", "application/json; charset=utf-8", err, sizeof(err) - 1);
        return;
    }

    static const char not_found[] = "not found\n";
    (void)server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", not_found, sizeof(not_found) - 1);
}

using EventSnapshots = std::array<std::shared_ptr<const std::string>, eventCount>;

struct WebSocketDelivery {
    EventSnapshots sent{};
    size_t nextTopic{};
    uint64_t blockedAt{};
};

static void pollWebSockets(HttpServer *server, WebSocketDelivery *delivery, uint64_t now) {
    auto &transport = *server->transport;
    EventSnapshots available;
    {
        std::lock_guard<std::mutex> lock(transport.mutex);
        available = transport.events;
    }
    uint32_t demand = 0;
    for (size_t i = 0; i < HTTP_WEBSOCKET_CLIENTS; ++i) {
        auto &client = server->websocketClients[i];
        auto &state = delivery[i];
        if (client.fd < 0) continue;
        // Drain output first so a peer's ping can use the frame buffer.
        if (!flushWebSocket(&client) ||
            (client.outputOffset == client.outputSize &&
             !client.closeAfterFlush && !receiveWebSocket(&client, now))) {
            closeWebSocket(&client);
            continue;
        }
        if (client.outputOffset != client.outputSize) {
            if (!state.blockedAt) state.blockedAt = now;
            if (now - state.blockedAt >= 10000) {
                closeWebSocket(&client);
                continue;
            }
        } else {
            state.blockedAt = 0;
        }
        if (client.closeAfterFlush && client.outputOffset == client.outputSize) {
            closeWebSocket(&client);
            continue;
        }
        if (client.pingAtMs && now >= client.pingAtMs && now - client.pingAtMs > 60000) {
            closeWebSocket(&client);
            continue;
        }
        if (!client.pingAtMs && now >= client.lastActivityMs &&
            now - client.lastActivityMs >= 20000 &&
            client.outputOffset == client.outputSize) {
            if (!queueWebSocketFrame(&client, 0x9, nullptr, 0)) {
                closeWebSocket(&client);
                continue;
            }
            client.pingAtMs = now;
        }
        demand |= client.topics;
        for (size_t topic = 0; topic < eventCount; ++topic) {
            if (!(client.topics & (1U << topic))) state.sent[topic].reset();
        }
        if (client.outputOffset != client.outputSize) continue;
        for (size_t offset = 0; offset < eventCount; ++offset) {
            const size_t topic = (state.nextTopic + offset) % eventCount;
            if (!(client.topics & (1U << topic)) || !available[topic] ||
                available[topic] == state.sent[topic]) continue;
            const auto &message = *available[topic];
            if (!queueWebSocketFrame(&client, 0x1, message.data(), message.size()) ||
                !flushWebSocket(&client)) {
                closeWebSocket(&client);
            } else {
                state.sent[topic] = available[topic];
                state.nextTopic = (topic + 1) % eventCount;
            }
            break;
        }
    }
    transport.demand.store(demand);
}

static void pollDesktopWebSocket(HttpServer *server, uint64_t now) {
    auto &client = server->desktopClient;
    if (client.fd < 0) return;
    bool ok = flushWebSocket(&client);
    if (ok && client.outputSize == client.outputOffset && !client.closeAfterFlush)
        ok = receiveWebSocket(&client, now, server);
    if (client.outputOffset != client.outputSize) {
        if (!client.blockedAtMs) client.blockedAtMs = now;
        if (now - client.blockedAtMs >= 10000) ok = false;
    } else client.blockedAtMs = 0;
    if (ok && !client.desktopIdentified && now - client.connectedAtMs > 5000) ok = false;
    if (ok && client.pingAtMs && now - client.pingAtMs > 10000) ok = false;
    if (ok && client.closeAfterFlush && client.outputSize == client.outputOffset) ok = false;
    if (ok && client.desktopIdentified) {
        std::lock_guard<std::mutex> lock(server->transport->mutex);
        auto &transport = *server->transport;
        if (transport.desktopCanDefault && !transport.defaultWatchSent &&
            client.outputSize == client.outputOffset && !client.closeAfterFlush) {
            const std::string watch = desktop::defaultWatch();
            ok = queueWebSocketFrame(&client, 0x1, watch.data(), watch.size());
            if (ok) transport.defaultWatchSent = true;
            if (ok) printf("[HTTP] Desktop default-sink watch requested\n");
        }
        if (ok && transport.defaultPending && now >= transport.defaultDeadline) {
            printf("[HTTP] [WARN] Desktop default-sink request timed out\n");
            transport.defaultState = DesktopRouteState::TimedOut;
            ok = false;
        } else if (ok && transport.defaultPending && !transport.defaultSent &&
                   client.outputSize == client.outputOffset && !client.closeAfterFlush) {
            const std::string command = desktop::defaultSet(transport.defaultId, transport.defaultRequested);
            ok = queueWebSocketFrame(&client, 0x1, command.data(), command.size());
            if (ok) {
                transport.defaultSent = true;
                transport.defaultDeadline = now + 10000;
            }
        }
        if (ok && transport.routePending && now >= transport.routeDeadline) {
            printf("[HTTP] [WARN] Desktop route request timed out\n");
            transport.routeState = DesktopRouteState::TimedOut;
            ok = false;
        } else if (ok && transport.routePending && !transport.routeSent &&
                   client.outputSize == client.outputOffset && !client.closeAfterFlush) {
            const std::string command = desktop::routeCommand(transport.route);
            ok = queueWebSocketFrame(&client, 0x1, command.data(), command.size());
            if (ok) {
                transport.routeSent = true;
                transport.routeDeadline = now + 10000;
            }
        }
        if (ok && transport.inventoryPending && now >= transport.inventoryDeadline) {
            printf("[HTTP] [WARN] Desktop discovery timed out\n");
            transport.inventoryState = DesktopInventoryState::TimedOut;
            ok = false;
        } else if (ok && transport.inventoryPending && !transport.inventorySent &&
                   client.outputSize == client.outputOffset && !client.closeAfterFlush) {
            const desktop::InventoryCommand request{
                transport.inventoryId, static_cast<uint32_t>(transport.inventory.size())};
            const std::string command = desktop::inventoryCommand(request);
            ok = queueWebSocketFrame(&client, 0x1, command.data(), command.size());
            if (ok) {
                transport.inventorySent = true;
                transport.inventoryDeadline = now + 10000;
            }
        }
        if (ok && transport.rulesPending && transport.rulesSent && now >= transport.rulesDeadline) {
            printf("[HTTP] [WARN] Desktop automatic routing update timed out\n");
            transport.rulesState = DesktopRulesState::Failed;
            ok = false;
        } else if (ok && transport.rulesPending && !transport.rulesSent &&
                   client.outputSize == client.outputOffset && !client.closeAfterFlush) {
            const size_t count = std::min<size_t>(400, transport.rules.size() - transport.rulesOffset);
            const bool done = transport.rulesOffset + count == transport.rules.size();
            const std::string command = desktop::encodeChunk("rules_chunk", transport.rulesId,
                static_cast<uint32_t>(transport.rulesOffset),
                std::string_view(transport.rules).substr(transport.rulesOffset, count), done);
            ok = queueWebSocketFrame(&client, 0x1, command.data(), command.size());
            if (ok) {
                transport.rulesExpected = transport.rulesOffset + count;
                transport.rulesSent = true;
                transport.rulesDeadline = now + 10000;
            }
        }
    }
    if (ok && !client.pingAtMs && now - client.lastActivityMs >= 5000 &&
        client.outputSize == client.outputOffset) {
        ok = queueWebSocketFrame(&client, 0x9, nullptr, 0);
        client.pingAtMs = now;
    }
    if (!ok) {
        {
            std::lock_guard<std::mutex> lock(server->transport->mutex);
            auto &transport = *server->transport;
            if (transport.defaultPending) {
                printf("[HTTP] [WARN] Desktop default-sink request lost its connection\n");
                if (transport.defaultState != DesktopRouteState::TimedOut)
                    transport.defaultState = DesktopRouteState::Disconnected;
            }
            transport.defaultPending = transport.defaultSent = false;
            if (transport.defaultKnown && !transport.defaultName.empty() &&
                !recordDesktopDefault(transport, "")) {
                printf("[HTTP] [WARN] Discarding queued desktop default-sink events after overflow\n");
                transport.defaultEvents.clear();
                recordDesktopDefault(transport, "");
            }
            transport.defaultKnown = false;
            transport.defaultName.clear();
            transport.defaultWatchSent = transport.desktopCanDefault = false;
            transport.desktopConnected = false;
            if (server->transport->routePending)
                printf("[HTTP] [WARN] Desktop route request lost its connection\n");
            if (server->transport->routePending &&
                server->transport->routeState != DesktopRouteState::TimedOut)
                server->transport->routeState = DesktopRouteState::Disconnected;
            server->transport->routePending = server->transport->routeSent = false;
            server->transport->desktopCanRoute = false;
            if ((server->transport->inventoryPending ||
                 server->transport->inventoryState == DesktopInventoryState::Succeeded) &&
                server->transport->inventoryState != DesktopInventoryState::TimedOut)
                server->transport->inventoryState = DesktopInventoryState::Disconnected;
            server->transport->inventoryPending = server->transport->inventorySent = false;
            server->transport->desktopCanDiscover = false;
            server->transport->inventory.clear();
            server->transport->desktopCanAutoRoute = false;
            server->transport->rulesPending = server->transport->rulesSent = false;
            server->transport->rulesState = DesktopRulesState::Unavailable;
            server->transport->autoStatus = -1;
        }
        server->desktopMedia.store(DesktopMedia::Unavailable);
        if (client.desktopIdentified) publishDesktopStatus(server);
        closeWebSocket(&client);
    }
}

static void publishWebSocketEvents(HttpServer *server, uint64_t now) {
    auto &transport = *server->transport;
    const uint32_t demand = transport.demand.load();
    for (size_t topic = 0; topic < eventCount; ++topic) {
        if (topic == desktopTopic) continue;
        if (!(demand & (1U << topic)) || now < transport.nextEvent[topic]) continue;
        transport.nextEvent[topic] = now + eventIntervals[topic];
        char data[HTTP_WEBSOCKET_FRAME_MAX];
        size_t dataLength = 0;
        if (!buildHttpEventJson(server, eventTopics[topic], data, sizeof(data), &dataLength)) continue;
        std::string message = "{\"type\":\"event\",\"topic\":\"";
        message += eventTopics[topic];
        message += "\",\"data\":";
        message.append(data, dataLength);
        message += '}';
        std::lock_guard<std::mutex> lock(transport.mutex);
        transport.events[topic] = std::make_shared<const std::string>(std::move(message));
    }
}

constexpr size_t httpConnectionLimit = 12;

struct HttpConnection {
    int fd{-1};
    std::shared_ptr<HttpExchange> request;
    uint64_t started{}, progress{};
    size_t remaining{}, sent{};
    off_t fileOffset{};
    bool parsed{}, queued{}, writing{};

    void reset() {
        if (fd >= 0) close(fd);
        if (request) request->disconnected.store(true);
        *this = {};
    }

    void reject(HttpServer *server, const char *status, const char *message) {
        exchange = request.get();
        server->sendResponse(-1, status, "text/plain; charset=utf-8", message, strlen(message));
        exchange = nullptr;
        request->complete.store(true);
        queued = true;
    }
};

static size_t requestBodyLimit(const char *method, const char *path) {
    if (!strcmp(method, "PUT") && !strcmp(path, "/api/config/restore"))
        return audiox::backup::maxArchiveSize;
    if (!strcmp(method, "PUT")) {
        if (!strcmp(path, HTTP_BOOT_LOADER_PATH) || !strcmp(path, HTTP_BOOT_KERNEL_PATH))
            return HTTP_BOOT_UPLOAD_MAX;
        if (!strcmp(path, HTTP_UPDATE_PATH)) return HTTP_UPDATE_MAX;
        if (!strcmp(path, HTTP_INITRAM_PATH)) return HTTP_INITRAM_MAX;
        if (!strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX))) return HTTP_ROOTFS_UPLOAD_MAX;
        if (!strcmp(path, "/api/plugins/install")) return 12 * 1024 * 1024;
    }
    return HTTP_BODY_MAX;
}

static int openUploadStage() {
    if (ensureDir(HTTP_STAGE_DIR, 0755) != RET_OK) return -1;
    return open(HTTP_STAGE_DIR, O_TMPFILE | O_RDWR | O_CLOEXEC, 0600);
}

static bool receiveHttp(HttpServer *server, HttpConnection &connection,
                        WebSocketDelivery *delivery, std::weak_ptr<HttpExchange> &largeUpload,
                        uint64_t now) {
    auto &request = *connection.request;
    char buffer[32768];
    const size_t want = connection.parsed ? std::min(connection.remaining, sizeof(buffer)) :
        std::min(HTTP_HEADER_MAX + 1 - request.header.size(), sizeof(buffer));
    const ssize_t n = recv(connection.fd, buffer, want, 0);
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return true;
    if (n == 0) {
        connection.reject(server, "400 Bad Request",
            connection.parsed ? "incomplete request body\n" : "incomplete request headers\n");
        return true;
    }
    if (n < 0) return false;
    connection.progress = now;
    const char *body = buffer;
    size_t length = static_cast<size_t>(n);
    if (!connection.parsed) {
        request.header.append(buffer, length);
        const ssize_t end = findHeaderEnd(request.header.data(), request.header.size());
        if (end < 0 || static_cast<size_t>(end) > HTTP_HEADER_MAX) {
            if (request.header.size() > HTTP_HEADER_MAX)
                connection.reject(server, "431 Request Header Fields Too Large", "request headers too large\n");
            return true;
        }
        HttpRequestHeaders headers{};
        char method[8], path[2048];
        bool http11 = false;
        if (!parseRequestLine(request.header.data(), end, method, sizeof(method), path, sizeof(path), &http11) ||
            !parseRequestHeaders(request.header.data(), end, &headers) || (http11 && !headers.hasHost)) {
            connection.reject(server, "400 Bad Request", "invalid request headers\n");
            return true;
        }
        char *query = strchr(path, '?');
        if (query) *query = '\0';
        request.api = !strncmp(path, "/api/", 5) || !strcmp(path, "/api");
        if (headers.hasHost && !allowedRequestHost(headers.host)) {
            connection.reject(server, "421 Misdirected Request", "unrecognized Host header\n");
            return true;
        }
        length = request.header.size() - end;
        if (!strcmp(path, "/ws") || !strcmp(path, "/ws/desktop")) {
            if (!strcmp(method, "GET") && !query && !length &&
                addWebSocketClient(server, connection.fd, headers, !strcmp(path, "/ws/desktop"))) {
                for (size_t i = 0; i < HTTP_WEBSOCKET_CLIENTS; ++i) {
                    if (server->websocketClients[i].fd == connection.fd) delivery[i] = {};
                }
                connection.fd = -1;
                return false;
            }
            connection.reject(server, "400 Bad Request", "invalid or unavailable websocket upgrade\n");
            return true;
        }
        const bool safe = !strcmp(method, "GET") || !strcmp(method, "HEAD") || !strcmp(method, "OPTIONS");
        const char *host = headers.hasHost ? headers.host : AUDIOX_USB_NET_SERVER_IP;
        if (!safe && ((headers.hasOrigin && !sameOriginHost(headers.origin, host)) ||
                      !strcasecmp(headers.fetchSite, "cross-site"))) {
            connection.reject(server, "403 Forbidden", "cross-origin request rejected\n");
            return true;
        }
        if (headers.contentLength > requestBodyLimit(method, path)) {
            connection.reject(server, "413 Payload Too Large", "payload too large\n");
            return true;
        }
        if (length > headers.contentLength) {
            connection.reject(server, "400 Bad Request", "unexpected request bytes\n");
            return true;
        }
        if (!strcmp(method, "PUT") && !strcmp(path, "/api/config/restore")) {
            exchange = &request;
            const int checked = checkRestoreStorage(server, -1, headers.contentLength);
            exchange = nullptr;
            if (checked != 0) {
                request.complete.store(true);
                connection.queued = true;
                return true;
            }
        }
        constexpr size_t prefixSize = sizeof(HTTP_ROOTFS_PREFIX) - 1;
        request.soundImport = !strcmp(method, "PUT") && !strncmp(path, HTTP_ROOTFS_PREFIX, prefixSize) &&
            isSfxUploadSuffix(path + prefixSize) && headers.contentLength != 0;
        if (headers.contentLength > HTTP_BODY_MAX || request.soundImport) {
            if (!largeUpload.expired()) {
                connection.reject(server, "503 Service Unavailable", "another upload is in progress; try again shortly\n");
                return true;
            }
            request.upload = openUploadStage();
            if (request.upload < 0) {
                printf("[HTTP] [ERR] upload staging failed: %s\n", strerror(errno));
                connection.reject(server, "507 Insufficient Storage", "could not stage upload\n");
                return true;
            }
            largeUpload = connection.request;
        }
        connection.parsed = true;
        connection.remaining = headers.contentLength;
        request.body.assign(request.header.data() + end, length);
        request.header.resize(end);
        body = request.body.data();
    }
    if (request.upload >= 0) {
        if (length && writeAllToFd(request.upload, body, length) < 0) {
            connection.reject(server, "507 Insufficient Storage", "could not write staged upload\n");
            return true;
        }
        request.body.clear();
    } else if (body == buffer) {
        request.body.append(body, length);
    }
    connection.remaining -= length;
    if (connection.remaining) return true;
    if (request.upload >= 0 && lseek(request.upload, 0, SEEK_SET) < 0) {
        connection.reject(server, "500 Internal Server Error", "could not rewind staged upload\n");
        return true;
    }
    auto &transport = *server->transport;
    std::lock_guard<std::mutex> lock(transport.mutex);
    if (transport.requests.size() >= httpConnectionLimit) {
        connection.reject(server, "503 Service Unavailable", "request queue full\n");
        return true;
    }
    transport.requests.push_back(connection.request);
    connection.queued = true;
    return true;
}

static bool transmitHttp(HttpConnection &connection, uint64_t now) {
    auto &request = *connection.request;
    ssize_t n;
    if (connection.sent < request.response.size()) {
        const size_t length = std::min<size_t>(65536, request.response.size() - connection.sent);
        n = send(connection.fd, request.response.data() + connection.sent, length, MSG_NOSIGNAL);
        if (n > 0) connection.sent += static_cast<size_t>(n);
    } else if (connection.fileOffset < request.downloadSize) {
        const off_t length = std::min<off_t>(65536, request.downloadSize - connection.fileOffset);
        n = sendfile(connection.fd, request.download, &connection.fileOffset, static_cast<size_t>(length));
    } else {
        return false;
    }
    if (n < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    if (!n) return false;
    connection.progress = now;
    return connection.sent < request.response.size() || connection.fileOffset < request.downloadSize;
}

static void *runHttpTransport(void *arg) {
    auto *server = static_cast<HttpServer *>(arg);
    auto &transport = *server->transport;
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGPIPE); // sendfile has no MSG_NOSIGNAL equivalent.
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    HttpConnection connections[httpConnectionLimit];
    WebSocketDelivery delivery[HTTP_WEBSOCKET_CLIENTS];
    std::weak_ptr<HttpExchange> largeUpload;
    publishDesktopStatus(server);
    while (!transport.stopping.load()) {
        struct pollfd fds[1 + httpConnectionLimit]{};
        fds[0] = {server->listen_fd, 0, 0};
        for (size_t i = 0; i < httpConnectionLimit; ++i) {
            auto &connection = connections[i];
            if (connection.fd < 0) fds[0].events = POLLIN;
            short events = POLLIN;
            if (connection.queued) {
                if (connection.request->complete.load()) {
                    if (!connection.writing) {
                        connection.writing = true;
                        connection.progress = httpMonotonicMs();
                    }
                    events = POLLOUT;
                } else {
                    events = connection.request->soundImport ? POLLRDHUP : 0;
                }
            }
            fds[i + 1] = {connection.fd, events, 0};
        }
        const int ready = ::poll(fds, 1 + httpConnectionLimit, 10);
        if (ready < 0) {
            if (errno == EINTR) continue;
            printf("[HTTP] [ERR] transport poll failed: %s\n", strerror(errno));
            break;
        }
        const uint64_t now = httpMonotonicMs();
        for (size_t i = 0; i < httpConnectionLimit; ++i) {
            auto &connection = connections[i];
            const short revents = fds[i + 1].revents;
            if (connection.fd < 0) continue;
            if (revents & (POLLERR | POLLHUP | POLLNVAL | POLLRDHUP)) {
                connection.reset();
            } else if (connection.writing) {
                if (now - connection.progress >= 10000 ||
                    ((revents & POLLOUT) && !transmitHttp(connection, now))) connection.reset();
            } else if (!connection.queued) {
                const uint64_t idleLimit = connection.parsed ? 10000 : 1500;
                const uint64_t absoluteLimit = connection.parsed ? 900000 : 5000;
                if (now - connection.progress >= idleLimit || now - connection.started >= absoluteLimit ||
                    ((revents & POLLIN) && !receiveHttp(server, connection, delivery, largeUpload, now))) {
                    connection.reset();
                }
            }
        }
        if (fds[0].revents & POLLIN) {
            for (auto &connection : connections) {
                if (connection.fd >= 0) continue;
                const int fd = accept4(server->listen_fd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
                if (fd < 0) break;
                connection.fd = fd;
                connection.started = connection.progress = now;
                connection.request = std::make_shared<HttpExchange>();
            }
        }
        pollWebSockets(server, delivery, now);
        pollDesktopWebSocket(server, now);
    }
    for (auto &connection : connections) connection.reset();
    for (auto &client : server->websocketClients) {
        if (client.fd >= 0) closeWebSocket(&client);
    }
    if (server->desktopClient.fd >= 0) closeWebSocket(&server->desktopClient);
    transport.demand.store(0);
    return nullptr;
}

} // namespace

const char *audioxDetectBoard() {
    return detectBoard();
}

HttpServer::HttpServer(Audiox *context) : app(context) {
    listen_fd = -1;
    started = 0;
    html_path[0] = '\0';
    body_len = 0;

    if (app) {
        app->http = this;
    }

    loadHtmlBody(this, HTTP_DEFAULT_HTML_PATH);
}

int HttpServer::requestDesktopRoute(uint32_t stream, uint32_t sink, uint32_t *id, uint32_t session) {
    if (!transport) return RET_WARN;
    std::lock_guard<std::mutex> lock(transport->mutex);
    if (!transport->desktopCanRoute || transport->routePending ||
        (session && session != transport->desktopSession)) return RET_WARN;
    if (++transport->routeSequence == 0) ++transport->routeSequence;
    transport->route = {transport->routeSequence, stream, sink};
    transport->routePending = true;
    transport->routeSent = false;
    transport->routeDeadline = httpMonotonicMs() + 10000;
    transport->routeState = DesktopRouteState::Pending;
    if (id) *id = transport->route.id;
    return RET_OK;
}

DesktopRouteState HttpServer::desktopRouteState(uint32_t id) const {
    if (!transport || !id) return DesktopRouteState::Unknown;
    std::lock_guard<std::mutex> lock(transport->mutex);
    return transport->route.id == id ? transport->routeState : DesktopRouteState::Unknown;
}

int HttpServer::requestDesktopDefault(const char *name, uint32_t *id, uint32_t session, bool userRequested) {
    if (!transport || !name || !desktop::validSinkName(name)) return RET_WARN;
    std::lock_guard<std::mutex> lock(transport->mutex);
    if (!transport->desktopCanDefault || !transport->defaultKnown || transport->defaultName.empty() ||
        transport->defaultPending || (session && session != transport->desktopSession)) return RET_WARN;
    if (++transport->defaultSequence == 0) ++transport->defaultSequence;
    transport->defaultId = transport->defaultSequence;
    transport->defaultRequested = name;
    transport->defaultUserRequested = userRequested;
    transport->defaultPending = true;
    transport->defaultSent = false;
    transport->defaultDeadline = httpMonotonicMs() + 10000;
    transport->defaultState = DesktopRouteState::Pending;
    if (id) *id = transport->defaultId;
    return RET_OK;
}

DesktopRouteState HttpServer::desktopDefaultState(uint32_t id) const {
    if (!transport || !id) return DesktopRouteState::Unknown;
    std::lock_guard<std::mutex> lock(transport->mutex);
    return transport->defaultId == id ? transport->defaultState : DesktopRouteState::Unknown;
}

bool HttpServer::desktopDefault(char (&name)[128], uint32_t session, uint32_t *currentSession,
                                bool *pending, DesktopDefaultAvailability *availability) const {
    name[0] = '\0';
    if (currentSession) *currentSession = 0;
    if (pending) *pending = false;
    if (availability) *availability = DesktopDefaultAvailability::Disconnected;
    if (!transport) return false;
    std::lock_guard<std::mutex> lock(transport->mutex);
    DesktopDefaultAvailability status = DesktopDefaultAvailability::Available;
    if (!transport->desktopConnected) status = DesktopDefaultAvailability::Disconnected;
    else if (!transport->desktopCanDefault) status = DesktopDefaultAvailability::Unsupported;
    else if (session && session != transport->desktopSession) status = DesktopDefaultAvailability::StaleSession;
    else if (!transport->defaultKnown) status = DesktopDefaultAvailability::Waiting;
    else if (transport->defaultName.empty()) status = DesktopDefaultAvailability::Unavailable;
    if (availability) *availability = status;
    if (status != DesktopDefaultAvailability::Available) return false;
    snprintf(name, sizeof(name), "%s", transport->defaultName.c_str());
    if (currentSession) *currentSession = transport->desktopSession;
    if (pending) *pending = transport->defaultPending;
    return true;
}

bool HttpServer::peekDesktopDefaultEvent(DesktopDefaultEvent &event) const {
    if (!transport) return false;
    std::lock_guard<std::mutex> lock(transport->mutex);
    if (transport->defaultEvents.empty()) return false;
    event = transport->defaultEvents.front();
    return true;
}

void HttpServer::acknowledgeDesktopDefaultEvent(uint64_t serial) {
    if (!transport) return;
    std::lock_guard<std::mutex> lock(transport->mutex);
    if (!transport->defaultEvents.empty() && transport->defaultEvents.front().serial == serial)
        transport->defaultEvents.pop_front();
}

int HttpServer::requestDesktopInventory(uint32_t session, uint32_t *id) {
    if (!transport || !session || !id) return RET_WARN;
    std::lock_guard<std::mutex> lock(transport->mutex);
    if (!transport->desktopCanDiscover || transport->inventoryPending ||
        session != transport->desktopSession) return RET_WARN;
    if (++transport->inventorySequence == 0) ++transport->inventorySequence;
    transport->inventoryId = transport->inventorySequence;
    transport->inventory.clear();
    transport->inventoryPending = true;
    transport->inventorySent = false;
    transport->inventoryDeadline = httpMonotonicMs() + 10000;
    transport->inventoryState = DesktopInventoryState::Pending;
    *id = transport->inventoryId;
    return RET_OK;
}

DesktopInventoryState HttpServer::desktopInventory(uint32_t id, std::string &nodes) const {
    if (!transport || !id) return DesktopInventoryState::Unknown;
    std::lock_guard<std::mutex> lock(transport->mutex);
    if (transport->inventoryId != id) return DesktopInventoryState::Unknown;
    if (transport->inventoryState == DesktopInventoryState::Succeeded)
        nodes = transport->inventory;
    return transport->inventoryState;
}

DesktopRulesState HttpServer::desktopRules(std::string &text, int *routeStatus) const {
    if (!transport) return DesktopRulesState::Unavailable;
    std::lock_guard<std::mutex> lock(transport->mutex);
    if (!transport->rulesValid) return DesktopRulesState::Failed;
    text = transport->rules;
    if (routeStatus) *routeStatus = transport->autoStatus;
    return transport->rulesState;
}

int HttpServer::updateDesktopRules(uint32_t session, const std::string &text) {
    if (!transport || !session) return RET_WARN;
    std::vector<desktop::Rule> parsed;
    if (!desktop::parseRules(text, parsed)) return RET_ERR;
    std::lock_guard<std::mutex> lock(transport->mutex);
    if (!transport->desktopCanAutoRoute || session != transport->desktopSession ||
        transport->rulesPending) return RET_WARN;
    if (!transport->rulesValid || !desktop::saveRules(text)) return RET_ERR;
    transport->rules = text;
    if (++transport->rulesId == 0) ++transport->rulesId;
    transport->rulesOffset = 0;
    transport->rulesPending = true;
    transport->rulesSent = false;
    transport->rulesState = DesktopRulesState::Pending;
    transport->autoStatus = -1;
    return RET_OK;
}

HttpServer::~HttpServer() {
    if (transport) {
        transport->stopping.store(true);
        if (transport->running) pthread_join(transport->thread, nullptr);
        auto &sound = transport->sound;
        if (sound.pid >= 0) {
            (void)kill(sound.pid, SIGKILL);
            while (waitpid(sound.pid, nullptr, 0) < 0 && errno == EINTR) {}
            unlink(sound.temporary.c_str());
        }
    }
    if (listen_fd >= 0) close(listen_fd);
    if (app && app->http == this) app->http = nullptr;
}

int HttpServer::startSocket() {
    if (started) {
        return RET_OK;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listen_fd < 0) {
        printf("[INIT] [ERR] HTTP socket failed: %s\n", strerror(errno));
        return RET_WARN;
    }

    int yes = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (setNonBlocking(listen_fd) < 0) {
        printf("[INIT] [ERR] HTTP fcntl non-blocking failed: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return RET_WARN;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    if (inet_pton(AF_INET, AUDIOX_USB_NET_SERVER_IP, &addr.sin_addr) != 1) {
        printf("[INIT] [ERR] invalid USB network server address: %s\n", AUDIOX_USB_NET_SERVER_IP);
        close(listen_fd);
        listen_fd = -1;
        return RET_WARN;
    }

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[INIT] [WARN] HTTP bind failed on port 80: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return RET_WARN;
    }

    if (listen(listen_fd, 4) < 0) {
        printf("[INIT] [WARN] HTTP listen failed: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return RET_WARN;
    }

    transport = std::make_unique<HttpTransport>();
    transport->rulesValid = desktop::loadRules(transport->rules);
    const int error = pthread_create(&transport->thread, nullptr, runHttpTransport, this);
    if (error) {
        printf("[INIT] [ERR] HTTP transport thread failed: %s\n", strerror(error));
        transport.reset();
        close(listen_fd);
        listen_fd = -1;
        return RET_WARN;
    }
    transport->running = true;
    started = 1;
    printf("[INIT] HTTP/WebSocket server listening on %s:80\n", AUDIOX_USB_NET_SERVER_IP);
    return RET_OK;
}

void HttpServer::poll() {
    if (!transport) return;
    const uint64_t now = httpMonotonicMs();
    if (transport->rebootAt && now >= transport->rebootAt) {
        transport->rebootAt = 0;
        if (reboot(transport->rebootCommand) < 0)
            printf("[HTTP] [ERR] reboot/shutdown failed: %s\n", strerror(errno));
    }
    pollSoundImport(this, now);
    std::shared_ptr<HttpExchange> request;
    {
        std::lock_guard<std::mutex> lock(transport->mutex);
        if (!transport->requests.empty()) {
            request = std::move(transport->requests.front());
            transport->requests.pop_front();
        }
    }
    if (request && !request->disconnected.load()) {
        exchange = request.get();
        handleClient(this, -1);
        exchange = nullptr;
        if (request->soundImport && transport->sound.pid >= 0 && !transport->sound.request) {
            transport->sound.request = request;
        } else {
            request->complete.store(true);
        }
    }
    publishWebSocketEvents(this, httpMonotonicMs());
}

void HttpServer::rebootAfterResponse(int command) {
    sync();
    if (exchange && transport) {
        transport->rebootCommand = command;
        transport->rebootAt = httpMonotonicMs() + 1000;
    } else {
        sleep(1);
        if (reboot(command) < 0) printf("[HTTP] [ERR] reboot/shutdown failed: %s\n", strerror(errno));
    }
}

bool HttpServer::canRespond(int cfd) const {
    return cfd >= 0 || exchange != nullptr;
}

int HttpServer::sendResponse(int cfd,
                             const char *status,
                             const char *content_type,
                             const void *body_data,
                             size_t body_size,
                             const char *download_name) const {
    return sendResponseWithHeaders(cfd, status, content_type, body_data, body_size, download_name, nullptr);
}

int HttpServer::sendResponseWithHeaders(int cfd, const char *status, const char *content_type,
                                       const void *body_data, size_t body_size,
                                       const char *download_name, const char *headers) const {
    if (!canRespond(cfd) || !status || !content_type) {
        return -1;
    }

    std::string normalizedError;
    if ((exchange ? exchange->api : apiRequest) && status[0] >= '4' && status[0] <= '5' &&
        body_data && body_size && content_type &&
        !strncasecmp(content_type, "text/plain", 10)) {
        normalizedError = "{\"ok\":false,\"error\":\"";
        normalizedError += audiox::json::escape(
            std::string(static_cast<const char *>(body_data), body_size).c_str());
        normalizedError += "\"}\n";
        body_data = normalizedError.data();
        body_size = normalizedError.size();
        content_type = "application/json; charset=utf-8";
    }

    char disposition[128] = {};
    if (download_name) {
        int n = snprintf(disposition, sizeof(disposition),
                         "Content-Disposition: attachment; filename=\"%s\"\r\nCache-Control: no-store\r\n", download_name);
        if (n <= 0 || (size_t)n >= sizeof(disposition)) return -1;
    }
    char hdr[2048];
    static constexpr char securityHeaders[] =
        "X-Content-Type-Options: nosniff\r\n"
        "X-Frame-Options: DENY\r\n"
        "Referrer-Policy: no-referrer\r\n";
    int hlen = snprintf(hdr,
                        sizeof(hdr),
                        "HTTP/1.1 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "%s"
                        "%s"
                        "%s"
                        "Connection: close\r\n\r\n",
                        status,
                        content_type,
                        body_size,
                        disposition,
                        securityHeaders,
                        headers ? headers : "");
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) {
        return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)hlen) {
        ssize_t n = writeResponse(cfd, hdr + sent, (size_t)hlen - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }

    const char *body_bytes = (const char *)body_data;
    size_t body_sent = 0;
    while (body_bytes && body_sent < body_size) {
        ssize_t n = writeResponse(cfd, body_bytes + body_sent, body_size - body_sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        body_sent += (size_t)n;
    }

    return 0;
}

int HttpServer::sendFilePath(int cfd,
                             const char *path,
                             const char *content_type_override) const {
    if (!path || !path[0]) {
        return -1;
    }

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return -1;
    }

    const char *ctype = content_type_override;
    if (!ctype) {
        const char *dot = strrchr(path, '.');
        if (!dot) {
            ctype = "application/octet-stream";
        } else if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
            ctype = "text/html; charset=utf-8";
        } else if (strcasecmp(dot, ".css") == 0) {
            ctype = "text/css; charset=utf-8";
        } else if (strcasecmp(dot, ".js") == 0) {
            ctype = "application/javascript; charset=utf-8";
        } else if (strcasecmp(dot, ".json") == 0) {
            ctype = "application/json; charset=utf-8";
        } else if (strcasecmp(dot, ".woff2") == 0) {
            ctype = "font/woff2";
        } else if (strcasecmp(dot, ".txt") == 0) {
            ctype = "text/plain; charset=utf-8";
        } else if (strcasecmp(dot, ".svg") == 0) {
            ctype = "image/svg+xml";
        } else if (strcasecmp(dot, ".png") == 0) {
            ctype = "image/png";
        } else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
            ctype = "image/jpeg";
        } else if (strcasecmp(dot, ".gif") == 0) {
            ctype = "image/gif";
        } else if (strcasecmp(dot, ".webp") == 0) {
            ctype = "image/webp";
        } else if (strcasecmp(dot, ".ico") == 0) {
            ctype = "image/x-icon";
        } else if (strcasecmp(dot, ".wav") == 0) {
            ctype = "audio/wav";
        } else {
            ctype = "application/octet-stream";
        }
    }

    if (sendResponse(cfd, "200 OK", ctype, NULL, (size_t)st.st_size) < 0) {
        close(fd);
        return -1;
    }
    if (exchange) {
        exchange->download = fd;
        exchange->downloadSize = st.st_size;
        return 0;
    }

    char buf[8192];
    while (1) {
        ssize_t nr = read(fd, buf, sizeof(buf));
        if (nr < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (nr == 0) {
            break;
        }

        size_t off = 0;
        while (off < (size_t)nr) {
            ssize_t nw = send(cfd, buf + off, (size_t)nr - off, 0);
            if (nw < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                return -1;
            }
            if (nw == 0) {
                close(fd);
                return -1;
            }
            off += (size_t)nw;
        }
    }

    close(fd);
    return 0;
}

int HttpServer::sendStaticFromWebRoot(int cfd, const char *path) const {
    if (!path || !path[0] || path[0] != '/') {
        return -1;
    }
    if (strstr(path, "..") != NULL || strchr(path, '\\') != NULL) {
        return -1;
    }

    char req_path[256];
    size_t req_len = strnlen(path, sizeof(req_path) - 1);
    memcpy(req_path, path, req_len);
    req_path[req_len] = '\0';

    char *query = strchr(req_path, '?');
    if (query) {
        *query = '\0';
    }

    char web_root[512];
    size_t root_len = strnlen(html_path, sizeof(web_root) - 1);
    if (root_len == 0) {
        return -1;
    }
    memcpy(web_root, html_path, root_len);
    web_root[root_len] = '\0';

    char *slash = strrchr(web_root, '/');
    if (!slash) {
        return -1;
    }
    *slash = '\0';

    if (!web_root[0]) {
        return -1;
    }

    char full[768];
    int n = snprintf(full, sizeof(full), "%s%s", web_root, req_path);
    if (n <= 0 || (size_t)n >= sizeof(full)) {
        return -1;
    }

    return sendFilePath(cfd, full, NULL);
}
