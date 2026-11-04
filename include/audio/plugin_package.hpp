#pragma once

#include "audio/plugin_api.h"
#include "defs.hpp"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace audiox::plugin {

inline constexpr const char *kDefaultRoot = ROOT_MOUNT_POINT "/plugins";
inline constexpr size_t kMaxPackageBytes = 12u * 1024u * 1024u;
inline constexpr size_t kMaxMetadataBytes = 4096;
inline constexpr size_t kMaxModuleBytes = 8u * 1024u * 1024u;
inline constexpr size_t kMaxAssetBytes = 256u * 1024u;
inline constexpr uint32_t kMaxAssets = 16;
inline constexpr uint32_t kMaxInstalled = 16;

struct ParameterSpec {
    char name[24]{};
    char label[64]{};
    float min = 0;
    float max = 0;
    float defaultValue = 0;
};

struct PackageMetadata {
    char id[24]{};
    char name[64]{};
    char version[32]{};
    uint32_t api = AUDIOX_PLUGIN_ABI_VERSION;
    uint32_t paramCount = 0;
    ParameterSpec params[AUDIOX_PLUGIN_MAX_PARAMS]{};
};

enum EntryKind : uint32_t { Metadata = 1, Module = 2, Asset = 3 };
struct Entry {
    uint32_t kind = 0;
    std::string name;
    std::vector<uint8_t> data;
};
struct Package {
    PackageMetadata metadata;
    std::vector<Entry> entries;
};

bool validId(const std::string &id);
bool validAssetName(const std::string &name);
bool parseMetadata(const std::string &text, PackageMetadata &metadata, std::string &error);
// Both supported machines can be inspected; install always requires native ELF.
bool parsePackage(const uint8_t *data, size_t size, Package &package, std::string &error,
                  bool requireNative = false);
bool buildPackage(const std::vector<Entry> &entries, std::vector<uint8_t> &data,
                  std::string &error);
bool installPackage(const uint8_t *data, size_t size, const std::string &root,
                    std::string &error);
bool listPackages(const std::string &root, std::vector<PackageMetadata> &packages,
                  std::string &error);
bool removePackage(const std::string &id, const std::string &root, std::string &error);
bool readPluginAsset(const std::string &id, const std::string &name,
                     const std::string &root, std::string &data, std::string &error);

} // namespace audiox::plugin
