#pragma once

#include <cstddef>

namespace audiox::config_storage {

inline constexpr const char *legacyNames[] = {
    "config.txt", "config.staging.txt", "routing.txt", "midi_map.txt",
    "fx_params.txt", "volumes.txt", "bluetooth", "scenes", "scripts",
    "telemetry-consent-v1.txt", "telemetry-consent-v2.txt", "telemetry-consent-v3.txt",
    "telemetry-installation-id.txt", "telemetry-queue-v1.txt", "telemetry-queue-v2.txt",
    "telemetry-queue-v3.txt"
};

int migrate(const char *root, char *error, size_t size);
int splitScripts(const char *configDir, char *error, size_t size);
int legacyCount(const char *root, char *error, size_t size);
int removeLegacy(const char *root, char *error, size_t size);

}
