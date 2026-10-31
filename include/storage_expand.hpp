#pragma once

#include <cstdint>
#include <cstring>

namespace audiox::storage {

inline uint32_t read32(const unsigned char *p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

// Only the shipped two-primary-partition DOS layout is eligible for growth.
// Preserve the partition start, boot code, disk identity, and all other entries.
inline bool planPartition(unsigned char (&mbr)[512], uint64_t diskSectors,
                          uint64_t kernelStart, uint64_t kernelSectors,
                          uint32_t *targetSectors) {
    if (mbr[510] != 0x55 || mbr[511] != 0xaa || diskSectors > UINT32_MAX ||
        diskSectors < 2) return false;
    const unsigned char *boot = mbr + 446;
    unsigned char *storage = mbr + 462;
    if ((boot[4] != 0x0b && boot[4] != 0x0c) ||
        (storage[4] != 0x83 && storage[4] != 0x0b && storage[4] != 0x0c) ||
        (boot[0] != 0 && boot[0] != 0x80) || storage[0] != 0) return false;
    for (unsigned i = 478; i < 510; ++i) if (mbr[i] != 0) return false;
    const uint64_t bootStart = read32(boot + 8);
    const uint64_t bootSize = read32(boot + 12);
    const uint64_t start = read32(storage + 8);
    const uint64_t size = read32(storage + 12);
    if (!bootStart || !bootSize || !size || bootStart + bootSize > start ||
        start != kernelStart || !kernelSectors || start + size > diskSectors ||
        kernelSectors > diskSectors - start || kernelSectors > size) return false;
    *targetSectors = static_cast<uint32_t>(diskSectors - start);
    for (unsigned i = 0; i < 4; ++i) storage[12 + i] = (*targetSectors >> (8 * i)) & 0xff;
    return true;
}

inline bool planFilesystem(const unsigned char (&super)[1024], uint64_t partitionBytes,
                           uint64_t *currentBlocks, uint64_t *targetBlocks) {
    if (super[56] != 0x53 || super[57] != 0xef || read32(super + 24) > 6) return false;
    const uint64_t blockSize = uint64_t(1024) << read32(super + 24);
    *currentBlocks = read32(super + 4);
    if (read32(super + 96) & 0x80) *currentBlocks |= uint64_t(read32(super + 336)) << 32;
    *targetBlocks = partitionBytes / blockSize;
    return *currentBlocks > 0 && *targetBlocks >= *currentBlocks;
}

int growPartition(const char *partition);
int growFilesystem(const char *partition, const char *mountPoint);

} // namespace audiox::storage
