#pragma once

#include <stddef.h>
#include <stdint.h>

namespace audiox {
namespace backup {

constexpr uint64_t maxArchiveSize = 4ULL * 1024 * 1024 * 1024;
constexpr uint32_t maxEntries = 8192;
constexpr size_t maxPath = 512;

// All functions report a human-readable error and return -1 on failure.
int create(const char *root, int archiveFd, char *error, size_t errorSize);
int stage(const char *root, int archiveFd, uint64_t size, char *error, size_t errorSize);
int applyPending(const char *root, char *error, size_t errorSize);

} // namespace backup
} // namespace audiox
