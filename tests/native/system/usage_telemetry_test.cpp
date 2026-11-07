#include "usage_telemetry.hpp"
#include <cassert>
#include <cstdio>
#include <fstream>
#include <unistd.h>

int main() {
    using namespace audiox::telemetry;
    static_assert(kCapacity == 8640 && kIntervalMs == 10000);
    IdleFilter filter;
    Point snapshot;
    assert(filter.keep(snapshot, false)); // First point is always retained.
    assert(!filter.keep(snapshot, false));
    snapshot.metrics[TemperatureC] = 50;
    snapshot.metrics[CpuPercent] = 10;
    assert(!filter.keep(snapshot, false));
    snapshot.metrics[AudioActive] = 1;
    assert(filter.keep(snapshot, false));
    assert(filter.keep(snapshot, false)); // Steady audio is not deduplicated.
    snapshot.metrics[AudioActive] = 0;
    assert(!filter.keep(snapshot, false));
    for (auto metric : {SoundboardTriggers, MidiNotes, MidiCc, SceneRecalls, Xruns}) {
        snapshot.metrics[metric] = 1;
        assert(filter.keep(snapshot, false));
        snapshot.metrics[metric] = 0;
        assert(!filter.keep(snapshot, false));
    }
    snapshot.metrics[UsbAudio] = 1;
    assert(filter.keep(snapshot, false));
    assert(!filter.keep(snapshot, false));
    snapshot.effects[3] = {1, 1};
    assert(filter.keep(snapshot, false));
    assert(filter.keep(snapshot, true)); // Parameters/routing/gain changes with identical counts.
    char directory[] = "/tmp/audiox-telemetry-XXXXXX";
    assert(mkdtemp(directory));
    Point point;
    point.version = "1.15.0-beta.13+test";
    point.board = 5;
    point.metrics[TemperatureC] = -1;
    point.metrics[CpuPercent] = -1;
    point.effects[3] = {2, 1};
    Store initial(directory);
    assert(initial.load() && initial.consent() == Consent::Unknown);
    assert(!initial.append(point) && initial.size() == 0);
    assert(initial.batch() == "{}");
    // Legacy opt-ins must explicitly accept public sharing; old data is not relabeled.
    {
        std::ofstream legacy(std::string(directory) + "/telemetry-consent-v1.txt");
        legacy << "1\n";
        std::ofstream queue(std::string(directory) + "/telemetry-queue-v1.txt");
        queue << "legacy data\n";
    }
    Store upgrade(directory);
    assert(upgrade.load() && upgrade.consent() == Consent::Unknown && upgrade.size() == 0);
    {
        std::ofstream legacy(std::string(directory) + "/telemetry-consent-v1.txt");
        legacy << "0\n";
    }
    Store legacyDeclined(directory);
    assert(legacyDeclined.load() && legacyDeclined.consent() == Consent::Disabled);
    {
        std::ofstream legacy(std::string(directory) + "/telemetry-consent-v2.txt");
        legacy << "1\n";
        std::ofstream queue(std::string(directory) + "/telemetry-queue-v2.txt");
        queue << "old private linked report\n";
    }
    Store linkedUpgrade(directory);
    assert(linkedUpgrade.load() && linkedUpgrade.consent() == Consent::Unknown && linkedUpgrade.size() == 0);
    {
        std::ofstream legacy(std::string(directory) + "/telemetry-consent-v2.txt");
        legacy << "0\n";
    }
    Store linkedDeclined(directory);
    assert(linkedDeclined.load() && linkedDeclined.consent() == Consent::Disabled);
    assert(initial.setConsent(false));
    assert(access((std::string(directory) + "/telemetry-queue-v1.txt").c_str(), F_OK) != 0);
    assert(access((std::string(directory) + "/telemetry-consent-v1.txt").c_str(), F_OK) != 0);
    assert(access((std::string(directory) + "/telemetry-consent-v2.txt").c_str(), F_OK) != 0);
    assert(access((std::string(directory) + "/telemetry-queue-v2.txt").c_str(), F_OK) != 0);
    Store declined(directory);
    assert(declined.load() && declined.consent() == Consent::Disabled);
    assert(declined.setConsent(true));
    for (unsigned i = 0; i < 99; ++i) assert(declined.append(point));
    assert(declined.size() == 99 && declined.batch() != "{}");
    assert(declined.append(point));
    const std::string firstBatch = declined.batch();
    const size_t idStart = firstBatch.find("\"installation_id\":\"") + 19;
    const std::string installationId = firstBatch.substr(idStart, 64);
    assert(installationId.size() == 64 && installationId.find_first_not_of("0123456789abcdef") == std::string::npos);
    assert(firstBatch.find("\"schema\":3") != std::string::npos);
    assert(firstBatch.find("\"public_consent\":1") != std::string::npos);
    assert(firstBatch.find("\"interval_ms\":10000") != std::string::npos);
    assert(declined.status().find("\"capacity\":8640") != std::string::npos);
    assert(declined.status().find("\"interval_ms\":10000") != std::string::npos);
    assert(firstBatch.find("\"points\":[") != std::string::npos);
    assert(firstBatch.find("\"temperature_c\":null") != std::string::npos);
    assert(firstBatch.find("\"effects\":[[0,0],[0,0],[0,0],[2,1]") != std::string::npos);
    const size_t tokenStart = firstBatch.find("\"batch\":\"") + 9;
    const std::string token = firstBatch.substr(tokenStart, 65);
    assert(token.size() == 65 && token[32] == ':');
    Store restarted(directory);
    assert(restarted.load() && restarted.size() == 100 && restarted.batch() == firstBatch);
    assert(!restarted.acknowledge("wrong"));
    assert(restarted.append(point));
    assert(restarted.acknowledge(token) && restarted.size() == 1);
    assert(!restarted.acknowledge(token));
    Store acknowledged(directory);
    assert(acknowledged.load() && acknowledged.size() == 1);
    for (unsigned i = 1; i < 120; ++i) assert(acknowledged.append(point));
    const auto countPoints = [](const std::string &json) {
        size_t count = 0, position = 0;
        while ((position = json.find("\"public_consent\":1", position)) != std::string::npos) { ++count; ++position; }
        return count;
    };
    assert(countPoints(acknowledged.batch()) == 120);
    for (unsigned i = 120; i < 500; ++i) assert(acknowledged.append(point));
    const std::string largeBatch = acknowledged.batch();
    assert(countPoints(largeBatch) == 500);
    assert(acknowledged.append(point));
    assert(acknowledged.acknowledge(largeBatch.substr(largeBatch.find("\"batch\":\"") + 9, 65)));
    assert(acknowledged.size() == 1); // A new point is not part of the in-flight batch.
    for (size_t i = 1; i < kCapacity; ++i) assert(acknowledged.append(point));
    assert(acknowledged.size() == kCapacity);
    assert(countPoints(acknowledged.batch()) == kUploadSize);
    const std::string overflowBatch = acknowledged.batch();
    const std::string oldToken = overflowBatch.substr(overflowBatch.find("\"batch\":\"") + 9, 65);
    assert(acknowledged.append(point));
    assert(acknowledged.size() == kCapacity - kBatchSize + 1);
    assert(!acknowledged.acknowledge(oldToken));
    Store bounded(directory);
    assert(bounded.load() && bounded.size() == kCapacity - kBatchSize + 1);
    assert(bounded.setConsent(false) && bounded.size() == 0 && bounded.batch() == "{}");
    assert(access((std::string(directory) + "/telemetry-installation-id.txt").c_str(), F_OK) != 0);
    Store optedOut(directory);
    assert(optedOut.load() && optedOut.size() == 0 && optedOut.consent() == Consent::Disabled);
    assert(optedOut.setConsent(true) && optedOut.size() == 0);
    for (unsigned i = 0; i < 100; ++i) assert(optedOut.append(point));
    assert(optedOut.batch().find(installationId) == std::string::npos);
    {
        std::ofstream corrupt(std::string(directory) + "/telemetry-queue-v3.txt");
        corrupt << "broken\n";
    }
    Store corrupt(directory);
    assert(!corrupt.load() && !corrupt.error().empty());
    assert(!corrupt.append(point) && corrupt.batch() == "{}");
    assert(!corrupt.setConsent(true));
    assert(corrupt.setConsent(false) && corrupt.error().empty());
    assert(corrupt.setConsent(true) && corrupt.append(point));
    assert(unlink((std::string(directory) + "/telemetry-installation-id.txt").c_str()) == 0);
    Store missingIdentity(directory);
    assert(!missingIdentity.load() && !missingIdentity.append(point));
    assert(corrupt.setConsent(false));
    {
        std::ofstream invalid(std::string(directory) + "/telemetry-consent-v3.txt");
        invalid << "yes\n";
    }
    Store invalid(directory);
    assert(!invalid.load() && invalid.consent() == Consent::Unknown);
    assert(invalid.setConsent(false));
    Store missing(std::string(directory) + "/nonexistent");
    assert(missing.load() && !missing.setConsent(true) && !missing.error().empty());
    assert(unlink((std::string(directory) + "/telemetry-consent-v3.txt").c_str()) == 0);
    assert(unlink((std::string(directory) + "/telemetry-queue-v3.txt").c_str()) == 0);
    assert(rmdir(directory) == 0);
    puts("usage telemetry persistence tests passed");
}
