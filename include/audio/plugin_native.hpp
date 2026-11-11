#pragma once

#include "audio/graph_native.hpp"

namespace audiox::plugin {
constexpr uint32_t kPluginNativeMagic = 0x41585032;
constexpr unsigned kMaxPluginSections = 128;
struct NativeSection {
    uint32_t offset{}, size{}, executable{};
};
struct PluginNativeHeader {
    uint32_t magic{}, size{}, entryOffset{}, sectionCount{};
    NativeSection sections[kMaxPluginSections]{};
};
} // namespace audiox::plugin
