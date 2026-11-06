#include "audio/device_health.hpp"
#include "http/diagnostic_summary.hpp"
#include <cassert>
#include <cstring>
#include <thread>

int main() {
    audiox::DeviceHealthMailbox mailbox;
    assert(mailbox.read().sampledAt == 0);
    audiox::DeviceHealthSnapshot snapshot{};
    snapshot.count = 1;
    snapshot.sampledAt = 123;
    snapshot.streams[0].xruns = 7;
    mailbox.publish(snapshot);
    assert(mailbox.read().streams[0].xruns == 7);
    assert(mailbox.read().sampledAt == 123);
    std::atomic<bool> finished{false};
    std::thread writer([&]() {
        for (unsigned n = 1; n < 10000; ++n) {
            snapshot.sampledAt = n;
            for (auto &row : snapshot.streams) row.xruns = n;
            mailbox.publish(snapshot);
        }
        finished.store(true, std::memory_order_release);
    });
    do {
        const auto &read = mailbox.read();
        for (const auto &row : read.streams) {
            if (read.sampledAt != 123) assert(row.xruns == read.sampledAt);
        }
    } while (!finished.load(std::memory_order_acquire));
    writer.join();
    const char logs[] = "[ERR] secret=do-not-export\n[WARN] underrun\nscript: private\n";
    const auto summary = audiox::summarizeDiagnosticLogs(logs, strlen(logs));
    assert(summary.lines == 3);
    assert(summary.errors == 1);
    assert(summary.warnings == 1);
    assert(summary.xruns == 1);
    assert(audiox::summarizeDiagnosticLogs("", 0).lines == 0);
}
