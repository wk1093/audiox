#include "usage_telemetry.hpp"
#include "defs.hpp"
#include "json.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sstream>
#include <sys/random.h>
#include <unistd.h>

namespace audiox::telemetry {

Store store(ROOT_MOUNT_POINT);

static bool hexId(const std::string &id, size_t length = 32) {
    return id.size() == length && id.find_first_not_of("0123456789abcdef") == std::string::npos;
}

static bool valid(const Point &point) {
    if (!hexId(point.id) || !hexId(point.installationId, 64) || point.version.empty() || point.version.size() > 96 ||
        point.version.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.+-") != std::string::npos ||
        (point.board != 0 && point.board != 4 && point.board != 5)) return false;
    for (size_t i = 0; i < MetricCount; ++i) {
        const auto value = point.metrics[i];
        if (value < ((i == TemperatureC || i == CpuPercent) ? -1 : 0) || value > UINT32_MAX) return false;
    }
    for (const auto &effect : point.effects)
        if (effect[0] > 4096 || effect[1] > effect[0]) return false;
    return true;
}

static std::string record(const Point &point) {
    std::ostringstream out;
    out << "3 " << point.id << ' ' << point.installationId << ' ' << point.version << ' ' << point.board;
    for (auto value : point.metrics) out << ' ' << value;
    for (const auto &effect : point.effects) out << ' ' << effect[0] << ' ' << effect[1];
    out << '\n';
    return out.str();
}

std::string Point::json() const {
    std::string out = "{\"id\":\"" + audiox::json::escape(id.c_str()) +
        "\",\"installation_id\":\"" + audiox::json::escape(installationId.c_str()) +
        "\",\"interval_ms\":" + std::to_string(kIntervalMs) +
        ",\"version\":\"" + audiox::json::escape(version.c_str()) +
        "\",\"public_consent\":1,\"board\":" + std::to_string(board) + ",\"metrics\":{";
    for (size_t i = 0; i < MetricCount; ++i) {
        if (i) out += ',';
        out += std::string("\"") + kMetricNames[i] + "\":";
        out += metrics[i] < 0 ? "null" : std::to_string(metrics[i]);
    }
    out += "},\"effects\":[";
    for (size_t i = 0; i < effects.size(); ++i) {
        if (i) out += ',';
        out += '[' + std::to_string(effects[i][0]) + ',' + std::to_string(effects[i][1]) + ']';
    }
    return out + "]}";
}

Store::Store(std::string directory) : directory_(std::move(directory)) {}

bool Store::fail(const char *operation) {
    error_ = std::string(operation) + ": " + strerror(errno);
    fprintf(stderr, "[TELEMETRY] [ERR] %s\n", error_.c_str());
    return false;
}

bool Store::atomicWrite(const std::string &path, const std::string &text) {
    const std::string temporary = path + ".tmp";
    const int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return fail("open temporary telemetry file");
    size_t offset = 0;
    bool ok = true;
    while (offset < text.size()) {
        const ssize_t count = write(fd, text.data() + offset, text.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        offset += static_cast<size_t>(count);
    }
    if (ok) ok = fsync(fd) == 0;
    int saved = errno;
    if (close(fd) < 0 && ok) { ok = false; saved = errno; }
    if (ok && rename(temporary.c_str(), path.c_str()) < 0) { ok = false; saved = errno; }
    if (ok) {
        const int dir = open(directory_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (dir < 0) { ok = false; saved = errno; }
        else {
            if (fsync(dir) < 0) { ok = false; saved = errno; }
            if (close(dir) < 0 && ok) { ok = false; saved = errno; }
        }
    }
    if (!ok) {
        if (unlink(temporary.c_str()) < 0 && errno != ENOENT)
            fprintf(stderr, "[TELEMETRY] [WARN] removing temporary file: %s\n", strerror(errno));
        errno = saved;
        return fail("persist telemetry");
    }
    return true;
}

bool Store::rewrite(const std::deque<Point> &points) {
    std::string text;
    for (const auto &point : points) text += record(point);
    return atomicWrite(directory_ + "/telemetry-queue-v3.txt", text);
}

bool Store::randomId(std::string &out, size_t bytes) {
    unsigned char random[32];
    if (bytes > sizeof(random)) { errno = EINVAL; return fail("invalid telemetry ID length"); }
    size_t offset = 0;
    while (offset < bytes) {
        const ssize_t count = getrandom(random + offset, bytes - offset, GRND_NONBLOCK);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return fail("generate random telemetry ID");
        offset += static_cast<size_t>(count);
    }
    constexpr char hex[] = "0123456789abcdef";
    out.clear();
    for (size_t i = 0; i < bytes; ++i) {
        out += hex[random[i] >> 4];
        out += hex[random[i] & 15];
    }
    return true;
}

bool Store::loadInstallationId() {
    FILE *file = fopen((directory_ + "/telemetry-installation-id.txt").c_str(), "r");
    if (!file) return fail("read telemetry installation ID; disable telemetry to reset");
    char text[66]{};
    const size_t count = fread(text, 1, sizeof(text), file);
    bool ok = !ferror(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) return fail("read telemetry installation ID");
    const std::string id(text, 64);
    if (count != 65 || text[64] != '\n' || !hexId(id, 64)) {
        errno = EINVAL;
        return fail("invalid telemetry installation ID; disable telemetry to reset");
    }
    installationId_ = id;
    return true;
}

bool Store::removeFile(const char *name) {
    if (unlink((directory_ + "/" + name).c_str()) < 0 && errno != ENOENT)
        return fail("remove obsolete telemetry file");
    return true;
}

bool Store::load() {
    if (loaded_) return error_.empty();
    loaded_ = true;
    const std::string path = directory_ + "/telemetry-consent-v3.txt";
    FILE *file = fopen(path.c_str(), "r");
    bool legacy = false;
    if (!file) {
        if (errno != ENOENT) return fail("read telemetry consent");
        file = fopen((directory_ + "/telemetry-consent-v2.txt").c_str(), "r");
        if (!file) {
            if (errno != ENOENT) return fail("read legacy telemetry consent");
            file = fopen((directory_ + "/telemetry-consent-v1.txt").c_str(), "r");
            if (!file) {
                if (errno == ENOENT) return true;
                return fail("read legacy telemetry consent");
            }
        }
        legacy = true;
    }
    const int decision = fgetc(file), newline = fgetc(file), end = fgetc(file);
    const bool readOk = !ferror(file);
    const int closed = fclose(file);
    if (!readOk || closed != 0) return fail("read telemetry consent");
    if ((decision != '0' && decision != '1') || newline != '\n' || end != EOF) {
        errno = EINVAL;
        return fail("invalid telemetry consent");
    }
    // An earlier opt-out remains an opt-out. Earlier opt-ins must accept public aggregates.
    consent_ = decision == '0' ? Consent::Disabled : legacy ? Consent::Unknown : Consent::Enabled;
    if (consent_ != Consent::Enabled) return true;
    if (!loadInstallationId()) return false;
    file = fopen((directory_ + "/telemetry-queue-v3.txt").c_str(), "r");
    if (!file) {
        if (errno == ENOENT) return true;
        return fail("read telemetry queue");
    }
    char line[2048];
    bool ok = true;
    while (fgets(line, sizeof(line), file)) {
        Point point;
        unsigned schema = 0;
        std::istringstream input(line);
        input >> schema >> point.id >> point.installationId >> point.version >> point.board;
        for (auto &value : point.metrics) input >> value;
        for (auto &effect : point.effects) input >> effect[0] >> effect[1];
        if (!input || schema != 3 || point.installationId != installationId_ || !strchr(line, '\n') || !valid(point) ||
            !(input >> std::ws).eof() || points_.size() == kCapacity) {
            ok = false;
            errno = EINVAL;
            break;
        }
        points_.push_back(std::move(point));
    }
    if (ferror(file)) ok = false;
    const int saved = errno;
    if (fclose(file) != 0 && ok) return fail("close telemetry queue");
    if (!ok) {
        points_.clear();
        errno = saved;
        return fail("invalid or unreadable telemetry queue; disable telemetry to clear it");
    }
    return true;
}

bool Store::setConsent(bool enabled) {
    if (!loaded_) load();
    if (enabled && !error_.empty() && consent_ == Consent::Enabled) {
        errno = EINVAL;
        return fail("disable telemetry to clear the queue before re-enabling");
    }
    // Discard old data before enabling, including data left by a failed opt-out cleanup.
    if (enabled && consent_ != Consent::Enabled) {
        if (!rewrite({}) || !randomId(installationId_, 32) ||
            !atomicWrite(directory_ + "/telemetry-installation-id.txt", installationId_ + "\n")) return false;
    }
    if (!atomicWrite(directory_ + "/telemetry-consent-v3.txt", enabled ? "1\n" : "0\n")) return false;
    const bool changed = consent_ != (enabled ? Consent::Enabled : Consent::Disabled);
    consent_ = enabled ? Consent::Enabled : Consent::Disabled;
    if (changed || !enabled) points_.clear();
    error_.clear();
    if (!enabled) {
        installationId_.clear();
        if (!removeFile("telemetry-installation-id.txt")) return false;
    }
    if (!removeFile("telemetry-queue-v1.txt") || !removeFile("telemetry-consent-v1.txt")) return false;
    if (!removeFile("telemetry-queue-v2.txt") || !removeFile("telemetry-consent-v2.txt")) return false;
    // Sync the directory after retiring legacy files / the opted-out identifier.
    if ((!enabled || changed) && !rewrite(points_)) return false;
    return true;
}

bool Store::append(Point point) {
    if (consent_ != Consent::Enabled || !error_.empty()) return false;
    if (!randomId(point.id, 16)) return false;
    point.installationId = installationId_;
    if (!valid(point)) { errno = EINVAL; return fail("invalid telemetry point"); }
    if (points_.size() == kCapacity) {
        auto retained = points_;
        retained.erase(retained.begin(), retained.begin() + kBatchSize);
        if (!rewrite(retained)) return false;
        points_ = std::move(retained);
        printf("[TELEMETRY] [WARN] queue full; discarded oldest 100 points\n");
    }
    const std::string text = record(point);
    const int fd = open((directory_ + "/telemetry-queue-v3.txt").c_str(),
                        O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return fail("append telemetry point");
    size_t written = 0;
    bool ok = true;
    while (written < text.size()) {
        const ssize_t count = write(fd, text.data() + written, text.size() - written);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { ok = false; break; }
        written += static_cast<size_t>(count);
    }
    if (ok) ok = fsync(fd) == 0;
    int saved = errno;
    if (close(fd) < 0 && ok) { ok = false; saved = errno; }
    if (!ok) { errno = saved; return fail("append telemetry point"); }
    points_.push_back(std::move(point));
    return true;
}

std::string Store::token() const {
    if (points_.size() < kBatchSize) return {};
    return points_.front().id + ":" + points_[kBatchSize - 1].id;
}

bool Store::acknowledge(const std::string &batch) {
    if (consent_ != Consent::Enabled || !error_.empty() || batch.empty() || batch != token()) return false;
    auto retained = points_;
    retained.erase(retained.begin(), retained.begin() + kBatchSize);
    if (!rewrite(retained)) return false;
    points_ = std::move(retained);
    return true;
}

std::string Store::status() const {
    return std::string("{\"consent\":\"") +
        (consent_ == Consent::Unknown ? "unknown" : consent_ == Consent::Enabled ? "enabled" : "disabled") +
        "\",\"consent_version\":3,\"pending\":" + std::to_string(points_.size()) + ",\"capacity\":" + std::to_string(kCapacity) +
        ",\"batch_size\":100,\"interval_ms\":" + std::to_string(kIntervalMs) + ",\"error\":\"" +
        audiox::json::escape(error_.c_str()) + "\"}";
}

std::string Store::batch() const {
    if (consent_ != Consent::Enabled || !error_.empty() || points_.size() < kBatchSize) return "{}";
    std::string out = "{\"schema\":3,\"batch\":\"" + token() + "\",\"points\":[";
    for (size_t i = 0; i < kBatchSize; ++i) {
        if (i) out += ',';
        out += points_[i].json();
    }
    return out + "]}";
}

} // namespace audiox::telemetry
