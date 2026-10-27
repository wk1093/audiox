#include <sys/reboot.h>
#include <cassert>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace {
int rebootRequests = 0;
int captureBackupReboot(int command) {
    assert(command == RB_AUTOBOOT);
    ++rebootRequests;
    return 0;
}
}

#define reboot captureBackupReboot
#include "../src/http/server.cpp"
#undef reboot

namespace fs = std::filesystem;

namespace {

std::string exchange(HttpServer &server, const char *method, const char *path,
                     const std::string &body = "", size_t claimed = 0) {
    int pair[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0);
    std::thread worker([&]() {
        size_t initial = body.empty() ? 0 : 1;
        (void)handleConfigBackup(&server, pair[1], method, path, body.data(), initial, claimed);
        close(pair[1]);
    });
    size_t sent = body.empty() ? 0 : 1;
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
    int cwd = open(".", O_RDONLY | O_DIRECTORY);
    assert(cwd >= 0);
    char pattern[] = "/tmp/audiox-backup-http-test-XXXXXX";
    assert(mkdtemp(pattern));
    assert(chdir(pattern) == 0);
    assert(mkdir("sfx", 0700) == 0);
    put("config.txt", "usb_sample_rate=44100\n");
    const std::string sound(200000, 'x');
    put("sfx/kick.wav", sound);
    HttpServer server(nullptr);
    assert(exchange(server, "POST", "/api/config/backup").find("405 Method Not Allowed") != std::string::npos);
    assert(exchange(server, "GET", "/api/config/restore").find("405 Method Not Allowed") != std::string::npos);
    assert(exchange(server, "PUT", "/api/config/restore", "", audiox::backup::maxArchiveSize + 1).find("413 Payload Too Large") != std::string::npos);
    std::string response = exchange(server, "GET", "/api/config/backup");
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
    put("config.txt", "new image");
    put("sfx/extra.wav", "not in backup");
    response = exchange(server, "PUT", "/api/config/restore", archive, archive.size());
    assert(response.find("202 Accepted") != std::string::npos);
    assert(response.find("validated and staged") != std::string::npos);
    assert(rebootRequests == 1);
    assert(get("config.txt") == "new image");
    char error[256];
    assert(audiox::backup::applyPending(".", error, sizeof(error)) == 0);
    assert(get("config.txt") == "usb_sample_rate=44100\n");
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
