#pragma once

#include "audio/effects/slot.hpp"
#include "audio/plugin_package.hpp"
#include "defs.hpp"

namespace audiox::plugin {

constexpr uint8_t TYPE_FIRST = 16;
constexpr uint8_t TYPE_COUNT = 16;
constexpr uint8_t TYPE_INVALID = 255;
constexpr const char *ROOT = ROOT_MOUNT_POINT "/plugins";

bool validTypeName(const char *name);
bool registerPackage(const PackageMetadata &metadata, std::string &error,
                     bool ui = false);
bool hasUi(uint8_t type);
uint8_t reserveType(const char *name);
uint8_t findType(const char *name);
bool isPluginType(uint8_t type);
bool available(uint8_t type);
const PackageMetadata *metadataFor(uint8_t type);
const effects::EffectTypeSpec *specFor(uint8_t type);
const char *nameFor(uint8_t type);
void markUnavailable(uint8_t type);
bool loadCatalog(std::string &error);

} // namespace audiox::plugin
