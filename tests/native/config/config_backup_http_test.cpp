#include <sys/reboot.h>
#include <cassert>
#include <filesystem>
#include <fstream>
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
#undef reboot
#undef statvfs

int prepareBootUpdateRecovery() { assert(false); return -1; }
int AudioContext::reloadSfxBank() { assert(false); return -1; }
static std::string uploadedPlugin;
int handleApiRequest(HttpServer *server, int cfd, const char *method, const char *path, const char *body, size_t size, const char *) {
    assert(!strcmp(method, "PUT") && !strcmp(path, "/api/plugins/install"));
    uploadedPlugin.assign(body, size);
    return server->sendResponse(cfd, "200 OK", "text/plain", "received", 8);
}
bool buildHttpEventJson(HttpServer *, const char *, char *, size_t, size_t *) { return false; }

namespace fs = std::filesystem;

namespace {

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

} // namespace

int main() {
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
    assert(fchdir(cwd) == 0);
    close(cwd);
    fs::remove_all(pattern);
    puts("PASS: native backup download, streamed restore, rejection, cleanup and staged reboot");
}
