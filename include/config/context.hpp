#pragma once

#include <cstddef>
#include <cstdint>
#include "defs.hpp"
#include "../context.hpp"

#define CONFIG_REAL_FILE_PATH ROOT_MOUNT_POINT "/config.txt"
#define CONFIG_STAGING_FILE_PATH ROOT_MOUNT_POINT "/config.staging.txt"
#define ROUTING_REAL_FILE_PATH ROOT_MOUNT_POINT "/routing.txt"

struct RouterConfig {
    RouterConfig();

    int getRouteCount() const;
    void getRoute(int index, char *out, size_t out_sz) const;
    void setRoute(int index, const char *route);
    void addRoute(const char *route);
    void removeRoute(int index);
};


struct ConfigData {
    uint32_t sampleRate;
    uint32_t playbackChannels;
    uint32_t captureChannels;
    uint32_t sampleSize;
};

struct ConfigStore {
    // For interacting with the config files
    Audiox *app;

    ConfigStore(Audiox *context);
    ConfigData readConfigFile() const;
    int writeConfigFile(ConfigData *cfg);

    RouterConfig router() const; // no need for a setter, the router config manages itself, this is just a handy accessor.

};
