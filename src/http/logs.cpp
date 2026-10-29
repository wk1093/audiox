#include "http/logs.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {

constexpr size_t kScanBytes = 2 * 1024 * 1024;
constexpr size_t kResponseBytes = 512 * 1024;
constexpr size_t kLineBytes = 2048;
struct Query {
    size_t limit = 600;
    std::string source = "both", level = "all", text;
};
struct Row {
    std::string id, source, level, text;
    bool truncated = false;
};
struct Tail {
    std::vector<Row> rows;
    size_t bytes = 0;
    bool more = false, scanLimited = false, missing = false;
};

std::string lower(std::string text) {
    for (char &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool decode(const std::string &text, std::string &out) {
    auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '%') {
            if (i + 2 >= text.size() || hex(text[i + 1]) < 0 || hex(text[i + 2]) < 0) return false;
            c = static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2]));
            i += 2;
        } else if (c == '+') c = ' ';
        if (static_cast<unsigned char>(c) < 32 || c == 127) return false;
        out += c;
    }
    return true;
}

bool parseQuery(const char *raw, Query &query) {
    const std::string input = raw ? raw : "";
    std::vector<std::string> seen;
    for (size_t start = 0; start < input.size();) {
        const size_t end = input.find('&', start);
        const std::string pair = input.substr(start, end == std::string::npos ? end : end - start);
        const size_t eq = pair.find('=');
        if (eq == std::string::npos) return false;
        const std::string key = pair.substr(0, eq);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) return false;
        seen.push_back(key);
        std::string value;
        if (!decode(pair.substr(eq + 1), value)) return false;
        if (key == "source") query.source = value;
        else if (key == "level") query.level = value;
        else if (key == "text") {
            if (value.size() > 160) return false;
            query.text = lower(value);
        } else if (key == "limit") {
            if (value.empty() || value.size() > 4 ||
                value.find_first_not_of("0123456789") != std::string::npos) return false;
            query.limit = 0;
            for (char c : value) query.limit = query.limit * 10 + static_cast<size_t>(c - '0');
        } else return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return query.limit >= 1 && query.limit <= 5000 &&
        (query.source == "both" || query.source == "stdout" || query.source == "stderr") &&
        (query.level == "all" || query.level == "debug" || query.level == "info" ||
         query.level == "warn" || query.level == "error");
}

int levelRank(const std::string &level) {
    if (level == "error") return 3;
    if (level == "warn") return 2;
    if (level == "info") return 1;
    return 0;
}

std::string detectLevel(const std::string &text) {
    if (text.find("error") != std::string::npos || text.find("[err]") != std::string::npos ||
        text.find("fatal") != std::string::npos) return "error";
    if (text.find("warn") != std::string::npos || text.find("[wrn]") != std::string::npos) return "warn";
    if (text.find("debug") != std::string::npos || text.find("[dbg]") != std::string::npos) return "debug";
    return "info";
}

std::string quoted(const std::string &text) {
    std::string out = "\"";
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < text.size(); ++i) {
        const unsigned char c = text[i];
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c < 32 || c == 127) {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else if (c >= 128) {
            const size_t count = c >= 0xc2 && c <= 0xdf ? 2 : c >= 0xe0 && c <= 0xef ? 3 :
                c >= 0xf0 && c <= 0xf4 ? 4 : 0;
            bool valid = count && i + count <= text.size();
            for (size_t j = 1; valid && j < count; ++j) {
                const unsigned char next = text[i + j];
                valid = next >= 0x80 && next <= 0xbf;
            }
            if (valid) {
                const unsigned char second = text[i + 1];
                valid = !(c == 0xe0 && second < 0xa0) && !(c == 0xed && second >= 0xa0) &&
                    !(c == 0xf0 && second < 0x90) && !(c == 0xf4 && second >= 0x90);
            }
            if (valid) { out.append(text, i, count); i += count - 1; }
            else out += "\\ufffd";
        } else out += c;
    }
    return out + '"';
}

int readTail(const char *source, const Query &query, Tail &tail) {
    const std::string path = std::string(ROOT_MOUNT_POINT) + "/" + source + ".log";
    const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT) { tail.missing = true; return 0; }
        return -errno;
    }
    struct stat st = {};
    int result = fstat(fd, &st) < 0 ? -errno : !S_ISREG(st.st_mode) ? -EINVAL : 0;
    off_t position = st.st_size;
    std::string reversed;
    auto emit = [&](off_t offset) {
        if (reversed.empty()) return;
        std::reverse(reversed.begin(), reversed.end());
        if (!reversed.empty() && reversed.back() == '\r') reversed.pop_back();
        const std::string folded = lower(reversed), level = detectLevel(folded);
        if ((query.level == "all" || levelRank(level) >= levelRank(query.level)) &&
            (query.text.empty() || folded.find(query.text) != std::string::npos)) {
            // Content hash distinguishes a file truncated and rewritten at the same offsets.
            uint64_t hash = 14695981039346656037ULL;
            for (unsigned char c : reversed) { hash ^= c; hash *= 1099511628211ULL; }
            tail.rows.push_back({std::string(source) + ":" + std::to_string(st.st_dev) + ":" +
                std::to_string(st.st_ino) + ":" + std::to_string(offset) + ":" + std::to_string(hash),
                source, level, reversed.substr(0, kLineBytes), reversed.size() > kLineBytes});
        }
        reversed.clear();
    };
    char buffer[8192];
    while (result == 0 && position > 0 && tail.bytes < kScanBytes && tail.rows.size() < query.limit) {
        const size_t length = std::min({sizeof(buffer), static_cast<size_t>(position), kScanBytes - tail.bytes});
        position -= static_cast<off_t>(length);
        size_t got = 0;
        while (got < length) {
            const ssize_t n = pread(fd, buffer + got, length - got, position + static_cast<off_t>(got));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { result = n < 0 ? -errno : -EAGAIN; break; }
            got += static_cast<size_t>(n);
        }
        tail.bytes += got;
        if (result != 0) break;
        for (size_t i = length; i > 0; --i) {
            if (buffer[i - 1] == '\n') {
                emit(position + static_cast<off_t>(i));
                if (tail.rows.size() == query.limit) { tail.more = position + static_cast<off_t>(i) > 0; break; }
            } else reversed += buffer[i - 1];
        }
    }
    if (result == 0 && position == 0 && tail.rows.size() < query.limit) emit(0);
    tail.scanLimited = position > 0 && tail.bytes >= kScanBytes && tail.rows.size() < query.limit;
    tail.more = tail.more || tail.scanLimited;
    if (close(fd) < 0 && result == 0) result = -errno;
    return result;
}

} // namespace

int handleSystemLogs(HttpServer *server, int cfd, const char *method, const char *rawQuery) {
    auto error = [&](const char *status, const char *text) {
        return server->sendResponse(cfd, status, "text/plain; charset=utf-8", text, strlen(text));
    };
    if (strcmp(method, "GET") != 0) return error("405 Method Not Allowed", "Use GET to read logs.\n");
    Query query;
    if (!parseQuery(rawQuery, query)) return error("400 Bad Request", "Invalid log filters. Use limit=1..5000, source=both|stdout|stderr, level=all|debug|info|warn|error and text (up to 160 bytes).\n");
    Tail out, err;
    int rc = query.source != "stderr" ? readTail("stdout", query, out) : 0;
    if (rc == 0 && query.source != "stdout") rc = readTail("stderr", query, err);
    if (rc < 0) {
        fprintf(stderr, "[HTTP] [WARN] log query failed: %s\n", strerror(-rc));
        return error(rc == -EAGAIN ? "503 Service Unavailable" : "500 Internal Server Error",
            "Could not read the log snapshot. Retry in a moment.\n");
    }
    // Files have no shared timestamps. Give both sources room rather than letting stderr hide stdout.
    size_t outCount = std::min(out.rows.size(), (query.limit + 1) / 2);
    size_t errCount = std::min(err.rows.size(), query.limit - outCount);
    outCount = std::min(out.rows.size(), query.limit - errCount);
    bool more = out.more || err.more || outCount < out.rows.size() || errCount < err.rows.size();
    std::vector<Row> rows;
    for (auto *tail : {&out, &err}) {
        const size_t count = tail == &out ? outCount : errCount;
        for (size_t i = count; i > 0; --i) rows.push_back(std::move(tail->rows[i - 1]));
    }
    std::vector<std::string> encodedRows;
    size_t encodedBytes = 0;
    size_t included = 0;
    // Keep the newest rows if unusually long lines hit the response byte budget.
    for (auto it = rows.rbegin(); it != rows.rend(); ++it) {
        const std::string row = "{\"id\":" + quoted(it->id) + ",\"source\":" + quoted(it->source) +
            ",\"level\":" + quoted(it->level) + ",\"line\":" + quoted(it->text) +
            ",\"truncated\":" + (it->truncated ? "true" : "false") + "}";
        if (encodedBytes + row.size() + 1024 > kResponseBytes) { more = true; break; }
        encodedBytes += row.size() + 1;
        encodedRows.push_back(row);
        ++included;
    }
    std::string encoded;
    encoded.reserve(encodedBytes);
    for (auto it = encodedRows.rbegin(); it != encodedRows.rend(); ++it) {
        if (!encoded.empty()) encoded += ',';
        encoded += *it;
    }
    const std::string response = "{\"rows\":[" + encoded + "],\"shown\":" + std::to_string(included) +
        ",\"has_more\":" + (more ? "true" : "false") +
        ",\"scan_limited\":" + (out.scanLimited || err.scanLimited ? "true" : "false") +
        ",\"bytes_read\":" + std::to_string(out.bytes + err.bytes) +
        ",\"missing_sources\":[" + (out.missing ? "\"stdout\"" : "") +
        (out.missing && err.missing ? "," : "") + (err.missing ? "\"stderr\"" : "") + "]}\n";
    return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", response.data(), response.size());
}
