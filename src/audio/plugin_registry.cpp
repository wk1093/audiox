#include "audio/plugin_registry.hpp"
#include <atomic>
#include <cstring>
#include <mutex>

namespace audiox::plugin {
namespace {
struct Type {
    char id[24]{};
    PackageMetadata metadata{};
    effects::EffectParamSpec params[effects::EFFECT_PARAM_MAX]{};
    effects::EffectTypeSpec spec{};
    effects::EffectTypeSpec placeholder{};
    std::atomic<bool> occupied{false};
    std::atomic<bool> loaded{false};
    std::atomic<bool> ui{false};
    std::atomic<bool> defined{false};
};
Type types[TYPE_COUNT];
std::mutex registrationMutex;
} // namespace

bool validTypeName(const char *name) {
    if (!name || strncmp(name, "plugin_", 7))
        return false;
    const size_t length = strnlen(name, 24);
    if (length < 8 || length >= 24 || name[7] < 'a' || name[7] > 'z')
        return false;
    for (size_t i = 8; i < length; ++i)
        if (!((name[i] >= 'a' && name[i] <= 'z') ||
              (name[i] >= '0' && name[i] <= '9') || name[i] == '_'))
            return false;
    return true;
}

bool isPluginType(uint8_t type) {
    return type >= TYPE_FIRST && type < TYPE_FIRST + TYPE_COUNT;
}
uint8_t findType(const char *name) {
    if (!validTypeName(name))
        return TYPE_INVALID;
    for (unsigned i = 0; i < TYPE_COUNT; ++i)
        if (types[i].occupied.load(std::memory_order_acquire) &&
            !strcmp(types[i].id, name))
            return static_cast<uint8_t>(TYPE_FIRST + i);
    return TYPE_INVALID;
}
uint8_t reserveType(const char *name) {
    if (!validTypeName(name))
        return TYPE_INVALID;
    std::lock_guard<std::mutex> lock(registrationMutex);
    uint8_t found = findType(name);
    if (found != TYPE_INVALID)
        return found;
    for (unsigned i = 0; i < TYPE_COUNT; ++i) {
        auto &type = types[i];
        if (type.occupied.load(std::memory_order_acquire))
            continue;
        snprintf(type.id, sizeof(type.id), "%s", name);
        type.spec.type = static_cast<uint8_t>(TYPE_FIRST + i);
        type.spec.params = type.params;
        type.placeholder.type = type.spec.type;
        type.occupied.store(true, std::memory_order_release);
        return type.spec.type;
    }
    return TYPE_INVALID;
}

bool registerPackage(const PackageMetadata &metadata, std::string &error,
                     bool ui) {
    const uint8_t index = reserveType(metadata.id);
    if (index == TYPE_INVALID) {
        error = "Plugin type registry is full or ID is invalid";
        return false;
    }
    std::lock_guard<std::mutex> lock(registrationMutex);
    auto &type = types[index - TYPE_FIRST];
    if (type.defined.load(std::memory_order_acquire)) {
        bool same = !strcmp(type.metadata.version, metadata.version) &&
                    !strcmp(type.metadata.name, metadata.name) &&
                    type.metadata.paramCount == metadata.paramCount;
        for (unsigned i = 0; same && i < metadata.paramCount; ++i) {
            const auto &a = type.metadata.params[i], &b = metadata.params[i];
            same = !strcmp(a.name, b.name) && !strcmp(a.label, b.label) &&
                   a.min == b.min && a.max == b.max &&
                   a.defaultValue == b.defaultValue;
        }
        if (!same) {
            error =
                "A different plugin version was already loaded; restart after "
                "replacing it";
            return false;
        }
        type.loaded.store(true, std::memory_order_release);
        type.ui.store(ui, std::memory_order_release);
        return true;
    }
    type.metadata = metadata;
    for (unsigned i = 0; i < metadata.paramCount; ++i) {
        const auto &param = type.metadata.params[i];
        type.params[i] = {param.name, param.label, param.min, param.max,
                          param.defaultValue};
    }
    type.spec.paramCount = static_cast<uint8_t>(metadata.paramCount);
    type.defined.store(true, std::memory_order_release);
    type.ui.store(ui, std::memory_order_release);
    type.loaded.store(true, std::memory_order_release);
    return true;
}
bool available(uint8_t type) {
    return isPluginType(type) &&
           types[type - TYPE_FIRST].loaded.load(std::memory_order_acquire);
}
bool hasUi(uint8_t type) {
    return isPluginType(type) &&
           types[type - TYPE_FIRST].ui.load(std::memory_order_acquire);
}
const PackageMetadata *metadataFor(uint8_t type) {
    return isPluginType(type) && types[type - TYPE_FIRST].defined.load(
                                     std::memory_order_acquire)
               ? &types[type - TYPE_FIRST].metadata
               : nullptr;
}
const effects::EffectTypeSpec *specFor(uint8_t type) {
    if (!isPluginType(type))
        return nullptr;
    const auto &entry = types[type - TYPE_FIRST];
    return entry.defined.load(std::memory_order_acquire) ? &entry.spec
                                                         : &entry.placeholder;
}
const char *nameFor(uint8_t type) {
    return isPluginType(type) && types[type - TYPE_FIRST].occupied.load(
                                     std::memory_order_acquire)
               ? types[type - TYPE_FIRST].id
               : "plugin_missing";
}
void markUnavailable(uint8_t type) {
    if (isPluginType(type))
        types[type - TYPE_FIRST].loaded.store(false, std::memory_order_release);
}
bool loadCatalog(std::string &error) {
    std::vector<PackageMetadata> catalog;
    if (!listPackages(ROOT, catalog, error))
        return false;
    for (const auto &metadata : catalog) {
        std::string asset, assetError;
        const bool ui =
            readPluginAsset(metadata.id, "index.html", ROOT, asset, assetError);
        if (!registerPackage(metadata, error, ui))
            return false;
    }
    return true;
}
} // namespace audiox::plugin
