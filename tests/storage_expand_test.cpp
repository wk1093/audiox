#include "storage_expand.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <string>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace {
std::string root;
uint64_t diskSectors = 128ULL * 1024 * 1024 * 1024 / 512;
uint64_t kernelSectors = 208 * 2048;
constexpr uint32_t start = 48 * 2048;
bool failKernelResize = false;
bool failFsResize = false;
int sectorSize = 512;
unsigned tableWrites = 0, kernelResizes = 0, filesystemResizes = 0;
ino_t diskInode = 0, partInode = 0, mountInode = 0;

void put32(unsigned char *p, uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) p[i] = (value >> (i * 8)) & 255;
}

void save(const char *name, const void *data, size_t size) {
    int fd = open((root + "/" + name).c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0);
    assert(write(fd, data, size) == (ssize_t)size);
    assert(close(fd) == 0);
}

void getMbr(unsigned char (&mbr)[512]) {
    int fd = open((root + "/disk").c_str(), O_RDONLY);
    assert(fd >= 0 && read(fd, mbr, sizeof(mbr)) == sizeof(mbr));
    close(fd);
}
}

extern "C" int __real_open(const char *, int, ...);
extern "C" int __wrap_open(const char *path, int flags, ...) {
    // Only production block paths are redirected; no real block device opens.
    if (strcmp(path, "/dev/audiox-test-disk") == 0) return __real_open((root + "/disk").c_str(), flags);
    if (strcmp(path, "/dev/audiox-test-disk2") == 0) return __real_open((root + "/partition").c_str(), flags);
    if (strncmp(path, "/dev/", 5) == 0) { errno = EACCES; return -1; }
    return __real_open(path, flags, 0600);
}

extern "C" char *__real_realpath(const char *, char *);
extern "C" char *__wrap_realpath(const char *path, char *out) {
    if (strcmp(path, "/sys/dev/block/240:2") == 0)
        return __real_realpath((root + "/audiox-test-disk/partition").c_str(), out);
    return __real_realpath(path, out);
}

extern "C" int __real_fstat(int, struct stat *);
extern "C" int __wrap_fstat(int fd, struct stat *info) {
    int rc = __real_fstat(fd, info);
    if (rc == 0 && (info->st_ino == diskInode || info->st_ino == partInode)) {
        info->st_mode = S_IFBLK | 0600;
        info->st_rdev = makedev(240, info->st_ino == diskInode ? 0 : 2);
    } else if (rc == 0 && info->st_ino == mountInode) info->st_dev = makedev(240, 2);
    return rc;
}

extern "C" ssize_t __real_pwrite(int, const void *, size_t, off_t);
extern "C" ssize_t __wrap_pwrite(int fd, const void *data, size_t count, off_t offset) {
    struct stat info{};
    assert(__real_fstat(fd, &info) == 0);
    if (info.st_ino == diskInode) {
        assert(offset == 474 && count == 4);
        ++tableWrites;
    }
    return __real_pwrite(fd, data, count, offset);
}

extern "C" int __wrap_ioctl(int fd, unsigned long request, void *arg) {
    struct stat info{};
    assert(__real_fstat(fd, &info) == 0);
    if (info.st_ino == diskInode) {
        if (request == BLKSSZGET) { *static_cast<int *>(arg) = sectorSize; return 0; }
        if (request == BLKGETSIZE64) { *static_cast<uint64_t *>(arg) = diskSectors * 512; return 0; }
        assert(request == BLKPG);
        auto *operation = static_cast<blkpg_ioctl_arg *>(arg);
        assert(operation->op == BLKPG_RESIZE_PARTITION);
        auto *change = static_cast<blkpg_partition *>(operation->data);
        assert(change->pno == 2 && change->start == uint64_t(start) * 512);
        ++kernelResizes;
        if (failKernelResize) { errno = EBUSY; return -1; }
        kernelSectors = change->length / 512;
        return 0;
    }
    if (info.st_ino == partInode) {
        assert(request == BLKGETSIZE64);
        *static_cast<uint64_t *>(arg) = kernelSectors * 512;
        return 0;
    }
    assert(info.st_ino == mountInode && request == _IOW('f', 16, uint64_t));
    ++filesystemResizes;
    if (failFsResize) { errno = EIO; return -1; }
    const uint64_t target = *static_cast<uint64_t *>(arg);
    assert(target <= kernelSectors / 8);
    int part = open((root + "/partition").c_str(), O_WRONLY);
    unsigned char count[4];
    put32(count, static_cast<uint32_t>(target));
    assert(__real_pwrite(part, count, 4, 1028) == 4);
    close(part);
    return 0;
}

int main() {
    char directory[] = "/tmp/audiox-storage-test-XXXXXX";
    assert(mkdtemp(directory));
    root = directory;
    unsigned char original[512]{};
    original[510] = 0x55; original[511] = 0xaa;
    original[446] = 0x80; original[450] = 0x0c;
    put32(original + 454, 2048);
    put32(original + 458, start - 2048);
    original[466] = 0x0c; // Existing images label ext4 as FAT32 in the MBR.
    put32(original + 470, start);
    put32(original + 474, static_cast<uint32_t>(kernelSectors));
    unsigned char mbr[512];
    uint32_t target = 0;
    memcpy(mbr, original, 512);
    assert(audiox::storage::planPartition(mbr, diskSectors, start, kernelSectors, &target));
    assert(target == diskSectors - start);
    assert(memcmp(mbr, original, 474) == 0 && memcmp(mbr + 478, original + 478, 34) == 0);
    assert(audiox::storage::planPartition(mbr, diskSectors, start, target, &target));
    mbr[466] = 0x83;
    assert(audiox::storage::planPartition(mbr, diskSectors, start, target, &target));
    assert(!audiox::storage::planPartition(mbr, UINT64_C(1) << 33, start, target, &target));
    for (unsigned offset : {510U, 450U, 466U, 478U}) {
        memcpy(mbr, original, 512);
        mbr[offset] = 0xee;
        assert(!audiox::storage::planPartition(mbr, diskSectors, start, kernelSectors, &target));
    }
    memcpy(mbr, original, 512);
    assert(!audiox::storage::planPartition(mbr, start + 1, start, kernelSectors, &target));
    assert(!audiox::storage::planPartition(mbr, diskSectors, start + 1, kernelSectors, &target));
    put32(mbr + 458, start);
    assert(!audiox::storage::planPartition(mbr, diskSectors, start, kernelSectors, &target));

    unsigned char geometry[1024]{};
    uint64_t current = 0, capacity = 0;
    geometry[56] = 0x53; geometry[57] = 0xef;
    put32(geometry + 4, 10);
    assert(audiox::storage::planFilesystem(geometry, 1024 * 20, &current, &capacity));
    assert(current == 10 && capacity == 20);
    assert(!audiox::storage::planFilesystem(geometry, 1024 * 9, &current, &capacity));
    put32(geometry + 24, 7);
    assert(!audiox::storage::planFilesystem(geometry, 1024 * 20, &current, &capacity));
    put32(geometry + 24, 2);
    put32(geometry + 96, 0x80);
    put32(geometry + 336, 1);
    assert(audiox::storage::planFilesystem(geometry, (UINT64_C(1) << 45), &current, &capacity));
    assert(current == (UINT64_C(1) << 32) + 10);
    geometry[56] = 0;
    assert(!audiox::storage::planFilesystem(geometry, (UINT64_C(1) << 45), &current, &capacity));

    assert(mkdir((root + "/audiox-test-disk").c_str(), 0700) == 0);
    assert(mkdir((root + "/audiox-test-disk/partition").c_str(), 0700) == 0);
    assert(mkdir((root + "/mounted").c_str(), 0700) == 0);
    save("audiox-test-disk/dev", "240:0\n", 6);
    save("audiox-test-disk/partition/partition", "2\n", 2);
    save("audiox-test-disk/partition/start", "98304\n", 6);
    const std::string sectors = std::to_string(kernelSectors);
    save("audiox-test-disk/partition/size", sectors.data(), sectors.size());
    save("disk", original, sizeof(original));
    unsigned char partData[2048]{};
    unsigned char *super = partData + 1024;
    super[56] = 0x53; super[57] = 0xef;
    put32(super + 24, 2); // 4096-byte blocks.
    put32(super + 4, static_cast<uint32_t>(kernelSectors / 8));
    save("partition", partData, sizeof(partData));
    struct stat info{};
    assert(stat((root + "/disk").c_str(), &info) == 0); diskInode = info.st_ino;
    assert(stat((root + "/partition").c_str(), &info) == 0); partInode = info.st_ino;
    assert(stat((root + "/mounted").c_str(), &info) == 0); mountInode = info.st_ino;

    sectorSize = 4096;
    assert(audiox::storage::growPartition("/dev/audiox-test-disk2") == -1);
    assert(tableWrites == 0 && kernelResizes == 0);
    sectorSize = 512;
    partData[1080] = 0;
    save("partition", partData, sizeof(partData));
    assert(audiox::storage::growPartition("/dev/audiox-test-disk2") == -1);
    assert(tableWrites == 0 && kernelResizes == 0);
    partData[1080] = 0x53;
    save("partition", partData, sizeof(partData));

    // Interrupted kernel update: persisted MBR is retried rather than rewritten.
    failKernelResize = true;
    assert(audiox::storage::growPartition("/dev/audiox-test-disk2") == -1);
    assert(tableWrites == 1 && kernelResizes == 1);
    failKernelResize = false;
    assert(audiox::storage::growPartition("/dev/audiox-test-disk2") == 0);
    assert(tableWrites == 1 && kernelResizes == 2);
    const auto expanded = std::to_string(kernelSectors);
    save("audiox-test-disk/partition/size", expanded.data(), expanded.size());
    assert(audiox::storage::growPartition("/dev/audiox-test-disk2") == 0);
    assert(tableWrites == 1 && kernelResizes == 2);
    getMbr(mbr);
    assert(memcmp(mbr, original, 474) == 0 && audiox::storage::read32(mbr + 474) == kernelSectors);
    failFsResize = true;
    assert(audiox::storage::growFilesystem("/dev/audiox-test-disk2", (root + "/mounted").c_str()) == -1);
    failFsResize = false;
    assert(audiox::storage::growFilesystem("/dev/audiox-test-disk2", (root + "/mounted").c_str()) == 0);
    assert(audiox::storage::growFilesystem("/dev/audiox-test-disk2", (root + "/mounted").c_str()) == 0);
    assert(filesystemResizes == 1 + (kernelSectors / 8 - 53248 + 262143) / 262144);
    for (const char *name : {"disk", "partition", "audiox-test-disk/dev",
                            "audiox-test-disk/partition/start", "audiox-test-disk/partition/size",
                            "audiox-test-disk/partition/partition"}) assert(unlink((root + "/" + name).c_str()) == 0);
    for (const char *name : {"audiox-test-disk/partition", "audiox-test-disk", "mounted"})
        assert(rmdir((root + "/" + name).c_str()) == 0);
    assert(rmdir(root.c_str()) == 0);
    puts("Storage expansion: preserved layout, unsupported layouts, retry, and idempotent filesystem growth passed");
}
