#include <sys/reboot.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <sys/statvfs.h>

namespace {
int rebootRequests = 0;
bool lowStorage = false;
int captureStatvfs(const char *path, struct statvfs *storage) {
    const int rc = statvfs(path, storage);
    if (rc == 0 && lowStorage) {
        storage->f_bavail = 1;
        storage->f_frsize = 4096;
    }
    return rc;
}
int captureBackupReboot(int command) {
    assert(command == RB_AUTOBOOT);
    ++rebootRequests;
    return 0;
}
}

#define reboot captureBackupReboot
#define statvfs(...) captureStatvfs(__VA_ARGS__)
#include "../../../src/http/server.cpp"
#include "../../../desktop/src/auto_routing.hpp"
#undef reboot
#undef statvfs

int prepareBootUpdateRecovery() { assert(false); return -1; }
int AudioContext::reloadSfxBank() { assert(false); return -1; }
static std::string uploadedPlugin;
int handleApiRequest(HttpServer *server, int cfd, const char *method, const char *path, const char *body, size_t size, const char *) {
    if (!strcmp(method, "GET") && !strcmp(path, "/api/test/download"))
        return server->sendFilePath(cfd, "download.bin", "application/octet-stream");
    assert(!strcmp(method, "PUT") && !strcmp(path, "/api/plugins/install"));
    uploadedPlugin.assign(body, size);
    return server->sendResponse(cfd, "200 OK", "text/plain", "received", 8);
}
bool buildHttpEventJson(HttpServer *, const char *, char *out, size_t capacity, size_t *length) {
    assert(capacity >= 3);
    memcpy(out, "{}", 3);
    *length = 2;
    return true;
}

namespace fs = std::filesystem;

namespace {
namespace direct_test {

std::string exchange(HttpServer &server, const char *method, const char *path,
                     const std::string &body = "", size_t claimed = 0) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    std::thread worker([&]() {
        handleClient(&server, pair[1]);
        close(pair[1]);
    });
    const std::string header = std::string(method) + " " + path + " HTTP/1.1\r\nHost: localhost\r\nContent-Length: " +
        std::to_string(claimed) + "\r\n\r\n";
    assert(send(pair[0], header.data(), header.size(), MSG_NOSIGNAL) == (ssize_t)header.size());
    size_t sent = 0;
    while (sent < body.size()) {
        ssize_t n = send(pair[0], body.data() + sent, body.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) break;
        sent += (size_t)n;
    }
    assert(shutdown(pair[0], SHUT_WR) == 0);
    std::string response;
    char buf[16384];
    for (;;) {
        ssize_t n = read(pair[0], buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, (size_t)n);
    }
    close(pair[0]);
    worker.join();
    return response;
}
}

std::string exchangeRaw(HttpServer &server, const std::string &request) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    std::thread worker([&]() {
        handleClient(&server, pair[1]);
        close(pair[1]);
    });
    assert(send(pair[0], request.data(), request.size(), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(request.size()));
    assert(shutdown(pair[0], SHUT_WR) == 0);
    std::string response;
    char buf[16384];
    for (;;) {
        ssize_t n = read(pair[0], buf, sizeof(buf));
        if (n <= 0) break;
        response.append(buf, static_cast<size_t>(n));
    }
    close(pair[0]);
    worker.join();
    return response;
}

void put(const char *path, const std::string &body) {
    std::ofstream out(path, std::ios::binary);
    out.write(body.data(), (std::streamsize)body.size());
    out.close();
    assert(out);
}

std::string get(const char *path) {
    std::ifstream in(path, std::ios::binary);
    assert(in);
    return std::string(std::istreambuf_iterator<char>(in), {});
}

void sendBytes(int fd, const std::string &bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        assert(n > 0);
        sent += static_cast<size_t>(n);
    }
}

void pumpUntil(HttpServer &server, const std::function<bool()> &done) {
    const uint64_t deadline = httpMonotonicMs() + 3000;
    while (!done()) {
        assert(httpMonotonicMs() < deadline);
        server.poll();
        usleep(1000);
    }
}

void testTransport(const char *upgrade, const std::string &badArchive) {
    desktop::Rule matchRule;
    matchRule.app = "usic";
    matchRule.target = "usb";
    matchRule.appRegex = true;
    matchRule.targetRegex = true;
    matchRule.pair = 1;
    std::vector<desktop::Rule> parsedRules;
    assert(desktop::parseRules(desktop::rulesText({matchRule}), parsedRules));
    assert(desktop::ruleMatches(parsedRules[0].app, true, "Music player"));
    assert(!desktop::ruleMatches(parsedRules[0].app, false, "Music player"));
    assert(desktop::virtualChannelsEnabled(desktop::rulesText({matchRule})));
    const std::string disabledRules = std::string(desktop::rulesHeader) +
        desktop::virtualChannelsOff + desktop::ruleLine(matchRule);
    assert(desktop::parseRules(disabledRules, parsedRules) && parsedRules.size() == 1);
    assert(!desktop::virtualChannelsEnabled(disabledRules));
    matchRule.app = "[";
    assert(!desktop::parseRules(desktop::rulesText({matchRule}), parsedRules));
    assert(!desktop::parseRules("# audiox desktop routes v2\n", parsedRules));
    std::string decoded;
    assert(desktop::jsonString(R"("music \u00e9 \ud83c\udfb5 \"\\")", decoded));
    assert(decoded == "music \xc3\xa9 \xf0\x9f\x8e\xb5 \"\\");
    assert(!desktop::jsonString(R"("\ud800")", decoded));
    HttpServer server(nullptr);
    server.listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    assert(server.listen_fd >= 0);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    assert(bind(server.listen_fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0);
    socklen_t localSize = sizeof(local);
    assert(getsockname(server.listen_fd, reinterpret_cast<sockaddr *>(&local), &localSize) == 0);
    assert(listen(server.listen_fd, 4) == 0);
    server.transport = std::make_unique<HttpTransport>();
    server.transport->rulesValid = desktop::loadRules(server.transport->rules);
    assert(server.transport->rulesValid);
    assert(pthread_create(&server.transport->thread, nullptr, runHttpTransport, &server) == 0);
    server.transport->running = true;

    const auto request = [&](const std::string &bytes, bool slowReader = false) {
        const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        assert(fd >= 0);
        if (slowReader) {
            const int size = 4096;
            assert(setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) == 0);
        }
        assert(connect(fd, reinterpret_cast<sockaddr *>(&local), sizeof(local)) == 0);
        sendBytes(fd, bytes);
        return fd;
    };
    const auto readUntil = [&](int fd, const std::function<bool(const std::string &)> &done) {
        std::string response;
        pumpUntil(server, [&]() {
            char buf[16384];
            const ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
            assert(n >= 0 || errno == EAGAIN || errno == EWOULDBLOCK);
            if (n > 0) response.append(buf, static_cast<size_t>(n));
            if (done(response)) return true;
            if (!n) fprintf(stderr, "unexpected response (%zu bytes): %.500s\n", response.size(), response.c_str());
            assert(n != 0);
            return false;
        });
        return response;
    };
    const auto expect = [&](int fd, const char *text) {
        readUntil(fd, [&](const std::string &response) { return response.find(text) != std::string::npos; });
        close(fd);
    };
    const std::string head = " HTTP/1.1\r\nHost: localhost\r\n";
    const std::string status = "GET /api/config/restore/check?size=20" + head + "\r\n";
    const std::string restore = "PUT /api/config/restore" + head +
        "Content-Length: " + std::to_string(badArchive.size()) + "\r\n\r\n";
    const int slowUpload = request(restore + badArchive.substr(0, 30));
    const std::string largeDownload(8 * 1024 * 1024, 'x');
    put("download.bin", largeDownload);
    const int slowDownload = request("GET /api/test/download" + head + "\r\n", true);
    readUntil(slowDownload, [](const auto &response) {
        return response.find("Content-Length: 8388608") != std::string::npos;
    });
    expect(request(status), "200 OK");
    sendBytes(slowUpload, badArchive.substr(30));
    expect(slowUpload, "checksum mismatch");
    close(slowDownload);
    const int download = request("GET /api/test/download" + head + "\r\n");
    const auto downloaded = readUntil(download, [&](const auto &response) {
        const size_t end = response.find("\r\n\r\n");
        return end != std::string::npos && response.size() == end + 4 + largeDownload.size();
    });
    assert(downloaded.substr(downloaded.find("\r\n\r\n") + 4) == largeDownload);
    close(download);

    const int ws = request(upgrade);
    readUntil(ws, [](const auto &response) { return response.find("\r\n\r\n") != std::string::npos; });
    const std::string subscription = R"({"type":"subscribe","topics":["audio.meters","midi.note","midi.cc","system.info","routing.things","desktop.status"]})";
    std::string frame{char(0x81), char(0x80 | subscription.size()), 1, 2, 3, 4};
    for (size_t i = 0; i < subscription.size(); ++i)
        frame += char(subscription[i] ^ frame[2 + i % 4]);
    sendBytes(ws, frame);
    readUntil(ws, [](const auto &response) {
        for (const char *topic : eventTopics) {
            if (response.find(std::string("\"topic\":\"") + topic + "\"") == std::string::npos) return false;
        }
        return true;
    });
    const std::string desktopUpgrade =
        "GET /ws/desktop HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\n"
        "Connection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: audiox.desktop.v1\r\n\r\n";
    const int desktop = request(desktopUpgrade);
    readUntil(desktop, [](const auto &response) {
        return response.find("Sec-WebSocket-Protocol: audiox.desktop.v1") != std::string::npos;
    });
    assert(server.requestDesktopRoute(11, 2) == RET_WARN);
    char defaultName[128]{};
    DesktopDefaultAvailability defaultAvailability{};
    assert(!server.desktopDefault(defaultName, 0, nullptr, nullptr, &defaultAvailability) &&
           defaultAvailability == DesktopDefaultAvailability::Disconnected);
    const std::string hello = R"({"type":"hello","hostname":"workstation","os":"linux","capabilities":["pipewire","default_sink","route_stream","pipewire_nodes","auto_route","desktop_update"]})";
    assert(parseDesktopHello(reinterpret_cast<const uint8_t *>(hello.data()), hello.size()));
    const std::string mediaHello = R"({"type":"hello","hostname":"workstation","os":"linux","capabilities":["mpris"],"media":"playing"})";
    assert(parseDesktopHello(reinterpret_cast<const uint8_t *>(mediaHello.data()), mediaHello.size()));
    const std::string badMedia = R"({"type":"hello","hostname":"workstation","os":"linux","capabilities":["mpris"],"media":"unknown"})";
    assert(!parseDesktopHello(reinterpret_cast<const uint8_t *>(badMedia.data()), badMedia.size()));
    const std::string duplicateMedia = R"({"type":"hello","hostname":"workstation","os":"linux","media":"playing","media":"paused","capabilities":[]})";
    assert(!parseDesktopHello(reinterpret_cast<const uint8_t *>(duplicateMedia.data()), duplicateMedia.size()));
    const std::string badHello = R"({"type":"hello","hostname":"workstation","os":"linux","capabilities":["bad\"value"]})";
    assert(!parseDesktopHello(reinterpret_cast<const uint8_t *>(badHello.data()), badHello.size()));
    assert(hello.size() >= 126 && hello.size() <= 65535);
    std::string helloFrame{char(0x81), char(0xfe), char(hello.size() >> 8), char(hello.size()), 1, 2, 3, 4};
    for (size_t i = 0; i < hello.size(); ++i) helloFrame += char(hello[i] ^ helloFrame[4 + i % 4]);
    sendBytes(desktop, helloFrame);
    readUntil(ws, [](const auto &response) {
        return response.find("\"hostname\":\"workstation\"") != std::string::npos &&
            response.find("\"default_sink\"") != std::string::npos;
    });
    const auto sendDesktopText = [&](int fd, const std::string &message) {
        assert(message.size() < 126);
        std::string packet{char(0x81), char(0x80 | message.size()), 1, 2, 3, 4};
        for (size_t i = 0; i < message.size(); ++i) packet += char(message[i] ^ packet[2 + i % 4]);
        sendBytes(fd, packet);
    };
    readUntil(desktop, [](const auto &response) {
        return response.find("\"type\":\"rules_chunk\"") != std::string::npos &&
               response.find("\"type\":\"default_watch\"") != std::string::npos;
    });
    uint32_t updateId = 0;
    assert(server.requestDesktopUpdate("v1.2.3+4", &updateId) == RET_OK);
    assert(server.requestDesktopUpdate("v1.2.3", &updateId) == RET_WARN);
    readUntil(desktop, [](const auto &response) {
        return response.find("\"type\":\"update_release\"") != std::string::npos;
    });
    sendDesktopText(desktop, desktop::updateResult(updateId, true));
    pumpUntil(server, [&]() { return server.desktopUpdateState(updateId) == DesktopRouteState::Succeeded; });
    readUntil(desktop, [](const auto &response) {
        return response.find("\"type\":\"update_ack\"") != std::string::npos;
    });
    assert(server.requestDesktopUpdate("../../invalid", &updateId) == RET_WARN);
    sendDesktopText(desktop, desktop::rulesAck(1, strlen(desktop::rulesHeader), true));
    std::string savedRules;
    pumpUntil(server, [&]() { return server.desktopRules(savedRules) == DesktopRulesState::Succeeded; });
    assert(!server.desktopDefault(defaultName, 0, nullptr, nullptr, &defaultAvailability) &&
           defaultAvailability == DesktopDefaultAvailability::Waiting);
    sendDesktopText(desktop, desktop::defaultSnapshot(""));
    pumpUntil(server, [&]() {
        return !server.desktopDefault(defaultName, 0, nullptr, nullptr, &defaultAvailability) &&
               defaultAvailability == DesktopDefaultAvailability::Unavailable;
    });
    DesktopDefaultEvent defaultEvent{};
    assert(server.peekDesktopDefaultEvent(defaultEvent));
    server.acknowledgeDesktopDefaultEvent(defaultEvent.serial);
    sendDesktopText(desktop, desktop::defaultSnapshot("sink.original"));
    pumpUntil(server, [&]() { return server.desktopDefault(defaultName); });
    assert(!strcmp(defaultName, "sink.original"));
    assert(server.peekDesktopDefaultEvent(defaultEvent) && defaultEvent.initial && defaultEvent.session == 1 &&
           !strcmp(defaultEvent.name, "sink.original"));
    server.acknowledgeDesktopDefaultEvent(defaultEvent.serial);
    sendDesktopText(desktop, desktop::defaultSnapshot("sink.external"));
    pumpUntil(server, [&]() { return server.peekDesktopDefaultEvent(defaultEvent); });
    assert(!defaultEvent.initial && !strcmp(defaultEvent.name, "sink.external"));
    server.acknowledgeDesktopDefaultEvent(defaultEvent.serial);
    uint32_t defaultId = 0;
    assert(server.requestDesktopDefault("sink.audiox", &defaultId, 2) == RET_WARN);
    assert(server.requestDesktopDefault("sink.audiox", &defaultId, 1) == RET_OK);
    uint32_t observedSession = 0;
    bool defaultPending = false;
    assert(server.desktopDefault(defaultName, 1, &observedSession, &defaultPending) &&
           observedSession == 1 && defaultPending);
    assert(server.requestDesktopDefault("sink.other") == RET_WARN);
    readUntil(desktop, [](const auto &response) {
        return response.find("\"type\":\"default_set\"") != std::string::npos;
    });
    sendDesktopText(desktop, desktop::defaultResult(defaultId, "sink.audiox", true));
    pumpUntil(server, [&]() { return server.desktopDefaultState(defaultId) == DesktopRouteState::Succeeded; });
    assert(server.desktopDefault(defaultName, 1, &observedSession, &defaultPending) && !defaultPending);
    assert(server.desktopDefault(defaultName) && !strcmp(defaultName, "sink.audiox"));
    assert(!server.peekDesktopDefaultEvent(defaultEvent));
    assert(server.requestDesktopDefault("sink.user", &defaultId, 1, true) == RET_OK);
    readUntil(desktop, [](const auto &response) {
        return response.find("\"type\":\"default_set\"") != std::string::npos;
    });
    sendDesktopText(desktop, desktop::defaultResult(defaultId, "sink.user", true));
    pumpUntil(server, [&]() { return server.desktopDefaultState(defaultId) == DesktopRouteState::Succeeded; });
    assert(server.peekDesktopDefaultEvent(defaultEvent) && !defaultEvent.initial &&
           !strcmp(defaultEvent.name, "sink.user"));
    server.acknowledgeDesktopDefaultEvent(defaultEvent.serial);
    assert(server.requestDesktopDefault("sink.missing", &defaultId, 1) == RET_OK);
    readUntil(desktop, [](const auto &response) {
        return response.find("\"type\":\"default_set\"") != std::string::npos;
    });
    sendDesktopText(desktop, desktop::defaultResult(defaultId, "sink.user", false));
    pumpUntil(server, [&]() { return server.desktopDefaultState(defaultId) == DesktopRouteState::Failed; });
    assert(server.desktopDefault(defaultName) && !strcmp(defaultName, "sink.user"));
    desktop::Rule newRule;
    newRule.app = "Audio player";
    newRule.target = "usb.sink";
    newRule.pair = 1;
    const std::string newRules = desktop::rulesText({newRule});
    assert(server.updateDesktopRules(2, newRules) == RET_WARN);
    assert(server.updateDesktopRules(1, newRules) == RET_OK);
    assert(get("config/desktop-routes.txt") == newRules);
    readUntil(desktop, [](const auto &response) { return response.find("\"type\":\"rules_chunk\"") != std::string::npos; });
    sendDesktopText(desktop, desktop::rulesAck(2, static_cast<uint32_t>(newRules.size()), true));
    pumpUntil(server, [&]() { return server.desktopRules(savedRules) == DesktopRulesState::Succeeded; });
    assert(savedRules == newRules);
    uint32_t inventoryId = 0;
    std::string inventory;
    assert(server.requestDesktopInventory(2, &inventoryId) == RET_WARN);
    assert(server.requestDesktopInventory(1, &inventoryId) == RET_OK);
    assert(server.desktopInventory(inventoryId, inventory) == DesktopInventoryState::Pending);
    readUntil(desktop, [](const auto &response) {
        return response.find(R"({"type":"inventory","id":1,"offset":0})") != std::string::npos;
    });
    const std::string nodes = R"([{"id":7,"node.name":"m\u00fasica"}])";
    sendDesktopText(desktop, desktop::inventoryChunk(inventoryId, 0, std::string_view(nodes).substr(0, 10), false));
    readUntil(desktop, [](const auto &response) {
        return response.find(R"({"type":"inventory","id":1,"offset":10})") != std::string::npos;
    });
    sendDesktopText(desktop, desktop::inventoryChunk(inventoryId, 10, std::string_view(nodes).substr(10), true));
    pumpUntil(server, [&]() {
        return server.desktopInventory(inventoryId, inventory) == DesktopInventoryState::Succeeded;
    });
    assert(inventory == nodes);
    uint32_t routeId = 0;
    assert(server.requestDesktopRoute(11, 2, &routeId, 2) == RET_WARN);
    assert(server.requestDesktopRoute(11, 2, &routeId, 1) == RET_OK);
    assert(routeId == 1 && server.desktopRouteState(routeId) == DesktopRouteState::Pending);
    assert(server.requestDesktopRoute(12, 3) == RET_WARN);
    readUntil(desktop, [](const auto &response) {
        return response.find(R"({"type":"route","id":1,"stream":11,"sink":2})") != std::string::npos;
    });
    const std::string routeResult = R"({"type":"route_result","id":1,"ok":false})";
    std::string resultFrame{char(0x81), char(0x80 | routeResult.size()), 1, 2, 3, 4};
    for (size_t i = 0; i < routeResult.size(); ++i) resultFrame += char(routeResult[i] ^ resultFrame[2 + i % 4]);
    sendBytes(desktop, resultFrame);
    pumpUntil(server, [&]() {
        std::lock_guard<std::mutex> lock(server.transport->mutex);
        return !server.transport->routePending;
    });
    assert(server.desktopRouteState(routeId) == DesktopRouteState::Failed);
    assert(server.requestDesktopRoute(11, 3, &routeId, 1) == RET_OK);
    readUntil(desktop, [](const auto &response) {
        return response.find(R"({"type":"route","id":2,"stream":11,"sink":3})") != std::string::npos;
    });
    const std::string successResult = R"({"type":"route_result","id":2,"ok":true})";
    std::string successFrame{char(0x81), char(0x80 | successResult.size()), 1, 2, 3, 4};
    for (size_t i = 0; i < successResult.size(); ++i) successFrame += char(successResult[i] ^ successFrame[2 + i % 4]);
    sendBytes(desktop, successFrame);
    pumpUntil(server, [&]() { return server.desktopRouteState(routeId) == DesktopRouteState::Succeeded; });
    std::string mediaFrame{char(0x81), char(0x80 | mediaHello.size()), 1, 2, 3, 4};
    for (size_t i = 0; i < mediaHello.size(); ++i) mediaFrame += char(mediaHello[i] ^ mediaFrame[2 + i % 4]);
    sendBytes(desktop, mediaFrame);
    readUntil(ws, [](const auto &response) {
        return response.find("\"media\":\"playing\"") != std::string::npos;
    });
    assert(server.desktopMedia.load() == DesktopMedia::Playing);
    assert(!server.desktopDefault(defaultName, 0, nullptr, nullptr, &defaultAvailability) &&
           defaultAvailability == DesktopDefaultAvailability::Unsupported);
    assert(server.desktopInventory(inventoryId, inventory) == DesktopInventoryState::Failed);
    expect(request(desktopUpgrade), "400 Bad Request");
    sendBytes(desktop, helloFrame);
    readUntil(ws, [](const auto &response) { return response.find("\"pipewire_nodes\"") != std::string::npos; });
    assert(server.requestDesktopInventory(1, &inventoryId) == RET_OK);
    close(desktop);
    readUntil(ws, [](const auto &response) { return response.find("\"connected\":false") != std::string::npos; });
    assert(server.desktopMedia.load() == DesktopMedia::Unavailable);
    assert(!server.desktopDefault(defaultName, 0, nullptr, nullptr, &defaultAvailability) &&
           defaultAvailability == DesktopDefaultAvailability::Disconnected);
    assert(server.desktopInventory(inventoryId, inventory) == DesktopInventoryState::Disconnected);
    const std::string browserDesktopUpgrade = desktopUpgrade.substr(0, desktopUpgrade.size() - 2) +
        "Origin: http://localhost\r\n\r\n";
    expect(request(browserDesktopUpgrade), "400 Bad Request");
    const int replacement = request(desktopUpgrade);
    readUntil(replacement, [](const auto &response) { return response.find("101 Switching Protocols") != std::string::npos; });
    sendBytes(replacement, helloFrame);
    readUntil(ws, [](const auto &response) { return response.find("\"hostname\":\"workstation\"") != std::string::npos; });
    readUntil(replacement, [](const auto &response) {
        return response.find(R"({"type":"rules_chunk","id":4,")") != std::string::npos;
    });
    sendDesktopText(replacement, desktop::rulesAck(4, static_cast<uint32_t>(newRules.size()), true));
    pumpUntil(server, [&]() { return server.desktopRules(savedRules) == DesktopRulesState::Succeeded; });
    assert(savedRules == newRules);
    while (server.peekDesktopDefaultEvent(defaultEvent))
        server.acknowledgeDesktopDefaultEvent(defaultEvent.serial);
    sendDesktopText(replacement, desktop::defaultSnapshot("sink.replacement"));
    uint32_t currentSession = 0;
    pumpUntil(server, [&]() { return server.desktopDefault(defaultName, 2, &currentSession); });
    assert(currentSession == 2 && !strcmp(defaultName, "sink.replacement"));
    assert(!server.desktopDefault(defaultName, 1));
    assert(server.requestDesktopDefault("sink.audiox", nullptr, 1) == RET_WARN);
    assert(server.peekDesktopDefaultEvent(defaultEvent) && defaultEvent.initial && defaultEvent.session == 2);
    server.acknowledgeDesktopDefaultEvent(defaultEvent.serial);
    assert(server.requestDesktopRoute(11, 4, &routeId, 1) == RET_WARN);
    assert(server.requestDesktopRoute(11, 4, &routeId, 2) == RET_OK);
    assert(server.desktopRouteState(2) == DesktopRouteState::Unknown);
    readUntil(replacement, [](const auto &response) {
        return response.find(R"({"type":"route","id":3,"stream":11,"sink":4})") != std::string::npos;
    });
    {
        std::lock_guard<std::mutex> lock(server.transport->mutex);
        server.transport->routeDeadline = 0;
    }
    readUntil(ws, [](const auto &response) { return response.find("\"connected\":false") != std::string::npos; });
    assert(server.desktopRouteState(routeId) == DesktopRouteState::TimedOut);
    assert(server.requestDesktopRoute(11, 4) == RET_WARN);
    close(replacement);
    if (const char *binary = getenv("AUDIOX_DESKTOP_BINARY")) {
        const pid_t child = fork();
        assert(child >= 0);
        if (child == 0) {
            const std::string port = std::to_string(ntohs(local.sin_port));
            execl(binary, binary, "--host", "127.0.0.1", "--port", port.c_str(), nullptr);
            _exit(127);
        }
        readUntil(ws, [](const auto &response) { return response.find("\"connected\":true") != std::string::npos; });
        if (getenv("AUDIOX_DESKTOP_DISCOVERY_TEST")) {
            uint32_t liveSession;
            {
                std::lock_guard<std::mutex> lock(server.transport->mutex);
                liveSession = server.transport->desktopSession;
            }
            uint32_t liveId;
            assert(server.requestDesktopInventory(liveSession, &liveId) == RET_OK);
            std::string liveNodes;
            pumpUntil(server, [&]() {
                const auto state = server.desktopInventory(liveId, liveNodes);
                assert(state == DesktopInventoryState::Pending || state == DesktopInventoryState::Succeeded);
                return state == DesktopInventoryState::Succeeded;
            });
            assert(liveNodes.find("\"node.name\":\"test_quad\"") != std::string::npos);
            std::string currentRules;
            pumpUntil(server, [&]() {
                return server.desktopRules(currentRules) == DesktopRulesState::Succeeded;
            });
            std::vector<desktop::PulseStream> beforePlayback, beforeCapture;
            assert(desktop::pulseStreams(-1, false, beforePlayback));
            assert(desktop::pulseStreams(-1, true, beforeCapture));
            const auto findStream = [](const std::vector<desktop::PulseStream> &streams, const char *name) {
                for (const auto &stream : streams) if (stream.app == name) return stream;
                assert(false);
                return desktop::PulseStream{};
            };
            const auto playback = findStream(beforePlayback, "AudioX isolated test");
            const auto capture = findStream(beforeCapture, "AudioX capture test");
            desktop::Rule playbackRule, captureRule;
            playbackRule.app = playback.app;
            playbackRule.target = "test_quad";
            playbackRule.pair = 1;
            captureRule.capture = true;
            captureRule.app = capture.app;
            captureRule.target = "test_quad.monitor";
            captureRule.pair = 1;
            assert(server.updateDesktopRules(liveSession, desktop::rulesText({playbackRule, captureRule})) == RET_OK);
            pumpUntil(server, [&]() { return server.desktopRules(currentRules) == DesktopRulesState::Succeeded; });
            const auto waitRoutes = [&](bool moved) {
                const uint64_t deadline = httpMonotonicMs() + 25000;
                for (;;) {
                    std::vector<desktop::PulseStream> currentPlayback, currentCapture;
                    assert(desktop::pulseStreams(-1, false, currentPlayback));
                    assert(desktop::pulseStreams(-1, true, currentCapture));
                    const auto p = findStream(currentPlayback, "AudioX isolated test");
                    const auto c = findStream(currentCapture, "AudioX capture test");
                    if ((p.destination != playback.destination) == moved &&
                        (c.destination != capture.destination) == moved) break;
                    if (httpMonotonicMs() >= deadline) {
                        std::fprintf(stderr, "desktop routes moved=%d playback=%u/%u capture=%u/%u\n",
                                     moved, p.destination, playback.destination,
                                     c.destination, capture.destination);
                        assert(false);
                    }
                    usleep(100000);
                }
            };
            waitRoutes(true);
            std::string links;
            assert(desktop::command("pw-link", {"-l"}, -1, links));
            const auto hasAutoLink = [&](const char *prefix, const char *suffix) {
                for (size_t pos = links.find(prefix); pos != std::string::npos; pos = links.find(prefix, pos + 1)) {
                    const size_t end = links.find('\n', pos);
                    const size_t found = links.find(suffix, pos);
                    if (end != std::string::npos && found != std::string::npos && found < end) return true;
                }
                return false;
            };
            assert(hasAutoLink("output.audiox_auto_", ":output_RL\n  |-> test_quad:playback_RL"));
            assert(hasAutoLink("output.audiox_auto_", ":output_RR\n  |-> test_quad:playback_RR"));
            assert(hasAutoLink("input.audiox_auto_", ":input_RL\n  |<- test_quad:monitor_RL"));
            assert(hasAutoLink("input.audiox_auto_", ":input_RR\n  |<- test_quad:monitor_RR"));
            assert(server.updateDesktopRules(liveSession, desktop::rulesHeader) == RET_OK);
            pumpUntil(server, [&]() { return server.desktopRules(currentRules) == DesktopRulesState::Succeeded; });
            waitRoutes(false);
        }
        assert(kill(child, SIGTERM) == 0);
        int status = 0;
        assert(waitpid(child, &status, 0) == child);
        readUntil(ws, [](const auto &response) { return response.find("\"connected\":false") != std::string::npos; });
    }
    close(ws);

    const std::string clip = "PUT /api/rootfs/sfx/kick.wav" + head + "Content-Length: 4\r\n\r\n";
    put("sfx/kick.wav", "original sound");
    put("ffmpeg", "#!/bin/sh\nexit 1\n");
    expect(request(clip + "data"), "415 Unsupported Media Type");
    assert(get("sfx/kick.wav") == "original sound");
    put("ffmpeg", "#!/bin/sh\nexec sleep 30\n");
    const int abort = request(clip + "data");
    pumpUntil(server, [&]() { return server.transport->sound.request != nullptr; });
    expect(request(status), "200 OK");
    expect(request(clip + "data"), "503 Service Unavailable");
    close(abort);
    pumpUntil(server, [&]() { return server.transport->sound.pid < 0; });
    assert(get("sfx/kick.wav") == "original sound");
    const int timedOut = request(clip + "data");
    pumpUntil(server, [&]() { return server.transport->sound.request != nullptr; });
    server.transport->sound.deadline = httpMonotonicMs();
    expect(timedOut, "504 Gateway Timeout");
    assert(get("sfx/kick.wav") == "original sound");
    const char pcm[] =
        "RIFF\x28\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0\x80\xbb\0\0"
        "\0\xee\x02\0\x04\0\x10\0data\x04\0\0\0\x01\0\x01\0";
    const std::string wav(pcm, sizeof(pcm) - 1);
    put("ffmpeg", "#!/bin/sh\nwhile [ \"$1\" != -i ]; do shift; done\ninput=$2\n"
                   "for output do :; done\nexec cp \"$input\" \"$output\"\n");
    const std::string wavRequest = "PUT /api/rootfs/sfx/kick.wav" + head +
        "Content-Length: " + std::to_string(wav.size()) + "\r\n\r\n" + wav;
    expect(request(wavRequest), "ok converted kick.wav");
    assert(get("sfx/kick.wav") == wav);
    if (const char *ffmpeg = getenv("AUDIOX_TEST_FFMPEG")) {
        assert(fs::remove("ffmpeg"));
        fs::create_symlink(ffmpeg, "ffmpeg");
        expect(request(wavRequest), "ok converted kick.wav");
        assert(get("sfx/kick.wav") == wav);
    }
    assert(fs::is_empty("staging"));
}

} // namespace

int main() {
    using direct_test::exchange;
    char accept[32];
    assert(audiox::websocket::acceptKey("dGhlIHNhbXBsZSBub25jZQ==", accept, sizeof(accept)));
    assert(!strcmp(accept, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="));
    assert(!audiox::websocket::acceptKey("not-a-websocket-key", accept, sizeof(accept)));
    assert(!audiox::websocket::acceptKey("dGhlIHNhbXBsZSBub25jZR==", accept, sizeof(accept)));
    const uint8_t partialFrame[] = {0x81, 0x82};
    audiox::websocket::Frame incomplete{};
    assert(audiox::websocket::decodeClientFrameResult(partialFrame, sizeof(partialFrame), &incomplete) ==
           audiox::websocket::FrameDecodeResult::Incomplete);
    const uint8_t invalidFrame[] = {0x01, 0x80};
    assert(audiox::websocket::decodeClientFrameResult(invalidFrame, sizeof(invalidFrame), &incomplete) ==
           audiox::websocket::FrameDecodeResult::Invalid);

    HttpRequestHeaders parsedHeaders{};
    const char validHeaders[] =
        "GET /ws HTTP/1.1\r\nHost: audiox.local\r\n"
        "Upgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: audiox.v1\r\n"
        "Origin: http://audiox.local\r\n\r\n";
    assert(parseRequestHeaders(validHeaders, sizeof(validHeaders) - 1, &parsedHeaders));
    assert(parsedHeaders.hasHost && !strcmp(parsedHeaders.host, "audiox.local"));
    assert(hasToken(parsedHeaders.connection, "upgrade"));
    assert(sameOriginHost(parsedHeaders.origin, parsedHeaders.host));
    assert(!sameOriginHost("", parsedHeaders.host));
    for (const char *invalid : {
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1\r\nContent-Length: 1\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nContent-Length: 1x\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nHost: y\r\n\r\n",
        "POST / HTTP/1.1\r\nHost: x\r\nSec-Fetch-Site: same-origin\r\nSec-Fetch-Site: cross-site\r\n\r\n",
        "GET / HTTP/1.1\r\nHost: x\r\nX-Test: bad\rvalue\r\n\r\n"})
        assert(!parseRequestHeaders(invalid, strlen(invalid), &parsedHeaders));
    assert(parseRequestHeaders(validHeaders, sizeof(validHeaders) - 1, &parsedHeaders));

    uint8_t maskedSubscribe[] = {
        0x81, 0x80 | 44, 1, 2, 3, 4,
        '{' ^ 1, '"' ^ 2, 't' ^ 3, 'y' ^ 4, 'p' ^ 1, 'e' ^ 2, '"' ^ 3, ':' ^ 4,
        '"' ^ 1, 's' ^ 2, 'u' ^ 3, 'b' ^ 4, 's' ^ 1, 'c' ^ 2, 'r' ^ 3, 'i' ^ 4,
        'b' ^ 1, 'e' ^ 2, '"' ^ 3, ',' ^ 4, '"' ^ 1, 't' ^ 2, 'o' ^ 3, 'p' ^ 4,
        'i' ^ 1, 'c' ^ 2, 's' ^ 3, '"' ^ 4, ':' ^ 1, '[' ^ 2, '"' ^ 3, 'a' ^ 4,
        'u' ^ 1, 'd' ^ 2, 'i' ^ 3, 'o' ^ 4, '.' ^ 1, 'm' ^ 2, 'e' ^ 3, 't' ^ 4,
        'e' ^ 1, 'r' ^ 2, 's' ^ 3, '"' ^ 4, ']' ^ 1, '}' ^ 2
    };
    // The test frame payload is deliberately small enough for a single masked frame.
    maskedSubscribe[1] = static_cast<uint8_t>(0x80 | (sizeof(maskedSubscribe) - 6));
    audiox::websocket::Frame decoded{};
    assert(audiox::websocket::decodeClientFrame(maskedSubscribe, sizeof(maskedSubscribe), &decoded));
    uint8_t subscription[1024];
    memcpy(subscription, decoded.payload, decoded.length);
    const size_t frameHeader = decoded.consumed - decoded.length;
    audiox::websocket::unmask(subscription, decoded.length, maskedSubscribe + frameHeader - 4);
    bool subscribe = false;
    uint32_t topics = 0;
    assert(parseWebSocketSubscription(subscription, decoded.length, &subscribe, &topics));
    assert(subscribe && topics == 1U);
    int websocketPair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, websocketPair) == 0);
    HttpServer websocketServer(nullptr);
    snprintf(websocketServer.requestHost, sizeof(websocketServer.requestHost), "%s", parsedHeaders.host);
    assert(addWebSocketClient(&websocketServer, websocketPair[0], parsedHeaders));
    char handshake[512]{};
    const ssize_t handshakeLength = read(websocketPair[1], handshake, sizeof(handshake) - 1);
    assert(handshakeLength > 0);
    assert(std::string(handshake, static_cast<size_t>(handshakeLength)).find(
        "101 Switching Protocols") != std::string::npos);
    assert(std::string(handshake, static_cast<size_t>(handshakeLength)).find(
        "Sec-WebSocket-Protocol: audiox.v1") != std::string::npos);
    closeWebSocket(&websocketServer.websocketClients[0]);
    close(websocketPair[1]);
    const char reboundHeaders[] =
        "GET /ws HTTP/1.1\r\nHost: attacker.example\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: audiox.v1\r\n"
        "Origin: http://attacker.example\r\n\r\n";
    HttpRequestHeaders rebound{};
    assert(parseRequestHeaders(reboundHeaders, sizeof(reboundHeaders) - 1, &rebound));
    snprintf(websocketServer.requestHost, sizeof(websocketServer.requestHost), "%s", AUDIOX_USB_NET_SERVER_IP);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, websocketPair) == 0);
    assert(!addWebSocketClient(&websocketServer, websocketPair[0], rebound));
    close(websocketPair[0]);
    close(websocketPair[1]);

    HttpServer httpServer(nullptr);
    const std::string originRejected = exchangeRaw(httpServer,
        "POST /api/system/shutdown HTTP/1.1\r\nHost: audiox.local\r\n"
        "Origin: http://attacker.example\r\nContent-Length: 0\r\n\r\n");
    assert(originRejected.find("403 Forbidden") != std::string::npos);
    assert(originRejected.find("cross-origin request rejected") != std::string::npos);
    const std::string reboundOriginRejected = exchangeRaw(httpServer,
        "POST /api/system/shutdown HTTP/1.1\r\nHost: attacker.example\r\n"
        "Origin: http://attacker.example\r\nContent-Length: 0\r\n\r\n");
    assert(reboundOriginRejected.find("421 Misdirected Request") != std::string::npos);
    const std::string fetchSiteRejected = exchangeRaw(httpServer,
        "POST /api/system/shutdown HTTP/1.1\r\nHost: audiox.local\r\n"
        "Sec-Fetch-Site: cross-site\r\nContent-Length: 0\r\n\r\n");
    assert(fetchSiteRejected.find("403 Forbidden") != std::string::npos);
    const std::string apiRouteError = exchangeRaw(httpServer,
        "POST /api/update HTTP/1.1\r\nHost: audiox.local\r\nContent-Length: 0\r\n\r\n");
    assert(apiRouteError.find("405 Method Not Allowed") != std::string::npos);
    assert(apiRouteError.find("Content-Type: application/json; charset=utf-8") != std::string::npos);
    assert(apiRouteError.find("\"ok\":false") != std::string::npos);

    int responsePair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, responsePair) == 0);
    httpServer.apiRequest = true;
    const char apiError[] = "bad \"request\"\n";
    assert(httpServer.sendResponseWithHeaders(responsePair[0], "400 Bad Request",
        "text/plain; charset=utf-8", apiError, sizeof(apiError) - 1, nullptr,
        "X-Test-Header: preserved\r\n") == 0);
    char responseBuffer[1024]{};
    const ssize_t responseLength = read(responsePair[1], responseBuffer, sizeof(responseBuffer) - 1);
    assert(responseLength > 0);
    const std::string responseText(responseBuffer, static_cast<size_t>(responseLength));
    assert(responseText.find("application/json; charset=utf-8") != std::string::npos);
    assert(responseText.find("\"error\":\"bad \\\"request\\\"\\u000a\"") != std::string::npos);
    assert(responseText.find("X-Test-Header: preserved") != std::string::npos);
    assert(responseText.find("X-Content-Type-Options: nosniff") != std::string::npos);
    close(responsePair[0]);
    close(responsePair[1]);

    int closePair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, closePair) == 0);
    HttpWebSocketClient closeClient{};
    closeClient.fd = closePair[0];
    const uint8_t maskedClose[] = {0x88, 0x82, 1, 2, 3, 4,
                                   static_cast<uint8_t>(0x03 ^ 1), static_cast<uint8_t>(0xE8 ^ 2)};
    assert(send(closePair[1], maskedClose, sizeof(maskedClose), MSG_NOSIGNAL) ==
           static_cast<ssize_t>(sizeof(maskedClose)));
    assert(receiveWebSocket(&closeClient, httpMonotonicMs()));
    assert(closeClient.closeAfterFlush);
    assert(flushWebSocket(&closeClient));
    uint8_t closeResponse[8]{};
    const ssize_t closeLength = read(closePair[1], closeResponse, sizeof(closeResponse));
    assert(closeLength == 4 && closeResponse[0] == 0x88 && closeResponse[1] == 2 &&
           closeResponse[2] == 0x03 && closeResponse[3] == 0xE8);
    closeWebSocket(&closeClient);
    close(closePair[1]);

    HttpServer desktopTimeout(nullptr);
    desktopTimeout.transport = std::make_unique<HttpTransport>();
    int desktopPair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, desktopPair) == 0);
    const uint64_t now = httpMonotonicMs();
    desktopTimeout.desktopClient.fd = desktopPair[0];
    desktopTimeout.desktopClient.connectedAtMs = now - 5001;
    desktopTimeout.desktopClient.lastActivityMs = now;
    pollDesktopWebSocket(&desktopTimeout, now);
    assert(desktopTimeout.desktopClient.fd < 0);
    close(desktopPair[1]);
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, desktopPair) == 0);
    desktopTimeout.desktopClient.fd = desktopPair[0];
    desktopTimeout.desktopClient.desktopIdentified = true;
    desktopTimeout.desktopClient.lastActivityMs = now - 5001;
    pollDesktopWebSocket(&desktopTimeout, now);
    assert(desktopTimeout.desktopClient.pingAtMs == now);
    pollDesktopWebSocket(&desktopTimeout, now + 10001);
    assert(desktopTimeout.desktopClient.fd < 0);
    assert(desktopTimeout.transport->events[desktopTopic]->find("\"connected\":false") != std::string::npos);
    close(desktopPair[1]);

    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    assert(cwd >= 0);
    char pattern[] = "/tmp/audiox-backup-http-test-XXXXXX";
    assert(mkdtemp(pattern));
    assert(chdir(pattern) == 0);
    assert(mkdir("config", 0700) == 0);
    assert(mkdir("sfx", 0700) == 0);
    put("config/config.txt", "usb_sample_rate=44100\n");
    const std::string sound(200000, 'x');
    put("sfx/kick.wav", sound);
    HttpServer server(nullptr);
    size_t length = 0;
    const auto checkedHeaders = [&](const char *headers) {
        HttpRequestHeaders parsed{};
        const std::string request = std::string("PUT /api/plugins/install HTTP/1.1\r\n") + headers + "\r\n";
        const bool ok = parseRequestHeaders(request.data(), request.size(), &parsed);
        if (ok) {
            length = parsed.contentLength;
            snprintf(server.requestHost, sizeof(server.requestHost), "%s", parsed.host);
        }
        return ok;
    };
    assert(checkedHeaders("Host: localhost:8766\r\nContent-Length: 200000\r\n"));
    assert(length == 200000 && !strcmp(server.requestHost, "localhost:8766"));
    for (const char *headers : {
        "Content-Length: 5junk\r\n", "Content-Length: -1\r\n",
        "Content-Length: 18446744073709551616\r\n",
        "Content-Length: 1\r\nContent-Length: 1\r\n",
        "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n",
        "Host: bad'host\r\nContent-Length: 1\r\n",
        "Host: first\r\nHost: second\r\nContent-Length: 1\r\n"})
        assert(!checkedHeaders(headers));
    const std::string binaryPlugin(200000, '\0');
    assert(exchange(server, "PUT", "/api/plugins/install", binaryPlugin, binaryPlugin.size()).find("200 OK") != std::string::npos);
    assert(uploadedPlugin == binaryPlugin);
    uploadedPlugin.clear();
    assert(exchange(server, "PUT", "/api/plugins/install", "partial", 200000).find("400 Bad Request") != std::string::npos);
    assert(uploadedPlugin.empty());
    assert(exchange(server, "PUT", "/api/plugins/install", "", 12 * 1024 * 1024 + 1).find("413 Payload Too Large") != std::string::npos);
    assert(exchange(server, "PUT", "/api/plugins/install", "", 0).find("413 Payload Too Large") != std::string::npos);
    assert(exchange(server, "POST", "/api/plugins/install", "", 0).find("405 Method Not Allowed") != std::string::npos);
    put("config/config.staging.txt", "original settings\n");
    const std::string staged = "usb_sample_rate=48000\n";
    assert(exchange(server, "PUT", "/api/rootfs/config.staging.txt", staged, staged.size())
        .find("200 OK") != std::string::npos);
    assert(get("config/config.staging.txt") == staged);
    assert(!fs::exists("config.staging.txt"));
    assert(exchange(server, "PUT", "/api/rootfs/config/config.staging.txt", "partial", 200000)
        .find("400 Bad Request") != std::string::npos);
    assert(get("config/config.staging.txt") == staged);
    put("config/config.staging.txt", "config_version=2\nfuture_setting=1\n");
    for (const char *path : {"/api/rootfs/config.staging.txt", "/api/rootfs/config/config.staging.txt"}) {
        assert(exchange(server, "PUT", path, staged, staged.size()).find("409 Conflict") != std::string::npos);
    }
    assert(get("config/config.staging.txt") == "config_version=2\nfuture_setting=1\n");
    assert(exchange(server, "PUT", "/api/rootfs/config//./.layout-version", "0\n", 2)
        .find("409 Conflict") != std::string::npos);
    put("config/config.staging.txt", staged);
    for (const auto &entry : fs::directory_iterator("config")) {
        assert(entry.path().filename().string().find(".tmp.") == std::string::npos);
    }
    assert(unlink("config/config.staging.txt") == 0);
    lowStorage = true;
    std::string response = exchange(server, "GET", "/api/config/restore/check?size=46661632");
    assert(response.find("507 Insufficient Storage") != std::string::npos);
    assert(response.find("need 110100480 bytes, have 4096 bytes free") != std::string::npos);
    assert(!fs::exists(".config-restore") && rebootRequests == 0);
    lowStorage = false;
    assert(exchange(server, "GET", "/api/config/restore/check?size=20").find("200 OK") != std::string::npos);
    for (const char *query : {"", "?size=nan", "?size=-1", "?size=20junk", "?size=18446744073709551616"})
        assert(exchange(server, "GET", (std::string("/api/config/restore/check") + query).c_str()).find("400 Bad Request") != std::string::npos);
    assert(exchange(server, "POST", "/api/config/restore/check?size=20").find("405 Method Not Allowed") != std::string::npos);
    assert(exchange(server, "POST", "/api/config/backup").find("405 Method Not Allowed") != std::string::npos);
    assert(exchange(server, "GET", "/api/config/restore").find("405 Method Not Allowed") != std::string::npos);
    assert(exchange(server, "PUT", "/api/config/restore", "", audiox::backup::maxArchiveSize + 1).find("413 Payload Too Large") != std::string::npos);
    response = exchange(server, "GET", "/api/config/backup");
    assert(response.find("200 OK") != std::string::npos);
    assert(response.find("Content-Disposition: attachment; filename=\"audiox-backup.axb\"") != std::string::npos);
    assert(response.find("Cache-Control: no-store") != std::string::npos);
    size_t start = response.find("\r\n\r\n");
    assert(start != std::string::npos);
    std::string archive = response.substr(start + 4);
    assert(archive.substr(0, 8) == "AXBACKUP");
    assert(archive.size() > 200000);
    assert(response.find("Content-Length: " + std::to_string(archive.size()) + "\r\n") != std::string::npos);
    int disconnected[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, disconnected) == 0);
    close(disconnected[0]);
    assert(handleConfigBackup(&server, disconnected[1], "GET", "/api/config/backup", "", 0, 0) < 0);
    close(disconnected[1]);
    std::string bad = archive;
    bad[bad.size() - 10] ^= 1;
    response = exchange(server, "PUT", "/api/config/restore", bad, bad.size());
    assert(response.find("400 Bad Request") != std::string::npos);
    assert(response.find("checksum mismatch") != std::string::npos);
    assert(!fs::exists(".config-restore"));
    response = exchange(server, "PUT", "/api/config/restore", archive.substr(0, 30), archive.size());
    assert(response.find("incomplete backup upload") != std::string::npos);
    assert(!fs::exists(".config-restore"));
    assert(rebootRequests == 0);
    put("config/config.txt", "new image");
    put("sfx/extra.wav", "not in backup");
    response = exchange(server, "PUT", "/api/config/restore", archive, archive.size());
    assert(response.find("202 Accepted") != std::string::npos);
    assert(response.find("validated and staged") != std::string::npos);
    assert(rebootRequests == 1);
    assert(get("config/config.txt") == "new image");
    char error[256];
    assert(audiox::backup::applyPending(".", error, sizeof(error)) == 0);
    assert(get("config/config.txt") == "usb_sample_rate=44100\n");
    assert(get("sfx/kick.wav") == sound);
    assert(!fs::exists("sfx/extra.wav"));
    for (const auto &entry : fs::directory_iterator(".")) {
        assert(entry.path().filename().string().find(".backup-transfer-") != 0);
    }
    testTransport(validHeaders, bad);
    assert(fchdir(cwd) == 0);
    close(cwd);
    fs::remove_all(pattern);
    puts("PASS: backup and restore, independent socket transfers, WebSocket topics, and atomic sound import");
}
