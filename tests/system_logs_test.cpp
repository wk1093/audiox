#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "../src/http/logs.cpp"

static std::string responseStatus, responseBody;
HttpServer::HttpServer(Audiox *context) : app(context) {}
int HttpServer::sendResponse(int, const char *status, const char *, const void *body, size_t size, const char *) const {
    responseStatus = status;
    responseBody.assign(static_cast<const char *>(body), size);
    return 0;
}

static void writeLog(const char *path, const std::string &text) {
    FILE *file = fopen(path, "w");
    assert(file);
    assert(fwrite(text.data(), 1, text.size(), file) == text.size());
    assert(fclose(file) == 0);
}

int main() {
    const int cwd = open(".", O_RDONLY | O_DIRECTORY);
    assert(cwd >= 0);
    char temp[] = "/tmp/audiox-logs-test-XXXXXX";
    assert(mkdtemp(temp) && chdir(temp) == 0);
    HttpServer server(nullptr);
    auto request = [&](const char *query, const char *status = "200 OK") {
        assert(handleSystemLogs(&server, 1, "GET", query) == 0);
        assert(responseStatus == status);
        assert(responseBody.size() < kResponseBytes);
    };
    request("");
    assert(responseBody.find("\"missing_sources\":[\"stdout\",\"stderr\"]") != std::string::npos);
    writeLog("stdout.log", "ordinary\n[DEBUG] details\n[WARN] USB \"one\"\n[ERR] USB failed\r\nlast partial");
    writeLog("stderr.log", "[WARN] stderr warning\n");
    Query query;
    assert(parseQuery("source=stdout&level=warn&text=uSb&limit=2", query));
    Tail tail;
    assert(readTail("stdout", query, tail) == 0 && tail.rows.size() == 2);
    assert(tail.rows[0].text == "[ERR] USB failed" && tail.rows[1].level == "warn");
    assert(tail.bytes < 8193);
    const auto id = tail.rows[0].id;
    Tail again;
    assert(readTail("stdout", query, again) == 0 && again.rows[0].id == id);
    request("source=stdout&level=warn&text=USB%20&limit=2");
    assert(responseBody.find("USB \\\"one\\\"") < responseBody.find("USB failed"));
    assert(responseBody.find("ordinary") == std::string::npos);
    request("limit=2");
    assert(responseBody.find("last partial") != std::string::npos);
    assert(responseBody.find("stderr warning") != std::string::npos);
    assert(responseBody.find("\"shown\":2") != std::string::npos);
    for (const char *bad : {"limit=0", "limit=5001", "limit=-1", "limit=10x", "limit=",
         "source=other", "level=trace", "text=%", "text=%00", "text=%0a", "path=stdout.log",
         "limit=2&limit=3"}) request(bad, "400 Bad Request");
    assert(handleSystemLogs(&server, 1, "POST", "") == 0 && responseStatus == "405 Method Not Allowed");
    std::string big;
    for (int i = 0; i < 100000; ++i) big += "[INFO] filler line " + std::to_string(i) + "\n";
    big += "[WARN] needle at end\n";
    writeLog("stdout.log", big);
    Query ten;
    ten.limit = 10;
    Tail tenRows;
    assert(readTail("stdout", ten, tenRows) == 0 && tenRows.rows.size() == 10);
    assert(tenRows.bytes <= 8192 && tenRows.more);
    Query missing;
    missing.text = "not-in-the-log";
    Tail scan;
    assert(readTail("stdout", missing, scan) == 0 && scan.rows.empty());
    assert(scan.bytes == kScanBytes && scan.scanLimited);
    writeLog("stdout.log", std::string(5000, 'x') + "end\n");
    Tail longLine;
    assert(readTail("stdout", ten, longLine) == 0 && longLine.rows.size() == 1);
    assert(longLine.rows[0].truncated && longLine.rows[0].text.size() == kLineBytes);
    writeLog("stdout.log", "[WARN] " + std::string(5000, 'x') + " search-at-end\n");
    Query longFilter;
    longFilter.level = "warn";
    longFilter.text = "search-at-end";
    Tail matchedLong;
    assert(readTail("stdout", longFilter, matchedLong) == 0 && matchedLong.rows.size() == 1);
    assert(matchedLong.rows[0].level == "warn" && matchedLong.rows[0].truncated);
    assert(quoted("UTF-8 \xc3\xa9") == "\"UTF-8 \xc3\xa9\"");
    assert(quoted("bad \xff") == "\"bad \\ufffd\"");
    writeLog("stdout.log", "[WARN] new\n");
    Tail rewritten;
    assert(readTail("stdout", Query{}, rewritten) == 0 && rewritten.rows[0].id != id);
    writeLog("stdout.log", std::string(2200000, '"') + "\n");
    request("source=stdout&limit=5000");
    assert(responseBody.find("\"scan_limited\":true") != std::string::npos);
    std::string largeRows;
    for (int i = 0; i < 1000; ++i) largeRows += std::string(2000, '"') + "\n";
    writeLog("stdout.log", largeRows);
    request("source=stdout&limit=5000");
    assert(responseBody.find("\"has_more\":true") != std::string::npos);
    assert(responseBody.size() <= kResponseBytes);
    assert(unlink("stderr.log") == 0 && mkdir("stderr.log", 0700) == 0);
    request("source=stderr", "500 Internal Server Error");
    assert(rmdir("stderr.log") == 0 && unlink("stdout.log") == 0);
    assert(fchdir(cwd) == 0);
    close(cwd);
    assert(rmdir(temp) == 0);
    puts("bounded log queries, filters, stable IDs, byte budgets and error tests passed");
}
