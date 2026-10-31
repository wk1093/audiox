#include "storage_expand.hpp"
#include "watchdog_heartbeat.hpp"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <linux/blkpg.h>
#include <linux/fs.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

namespace audiox::storage {
namespace {
struct Fd {
    int value;
    explicit Fd(int fd) : value(fd) {}
    ~Fd() { if (value >= 0) close(value); }
};

int fail(const char *operation, const char *path) {
    printf("[STORAGE] [WARN] %s (%s): %s; storage expansion will retry next boot\n",
           operation, path, strerror(errno));
    return -1;
}

bool readNumber(const char *path, unsigned long long *number) {
    FILE *file = fopen(path, "r");
    if (!file) return false;
    errno = 0;
    const bool ok = fscanf(file, "%llu", number) == 1;
    const int saved = errno;
    const int rc = fclose(file);
    if (!ok) errno = saved ? saved : EINVAL;
    return ok && rc == 0;
}
}

int growPartition(const char *partition) {
    Fd part(open(partition, O_RDONLY | O_CLOEXEC));
    struct stat partStat{};
    if (part.value < 0 || fstat(part.value, &partStat) < 0) return fail("open storage partition", partition);
    if (!S_ISBLK(partStat.st_mode)) { errno = EINVAL; return fail("not a block device", partition); }
    char link[128], sysPath[4096];
    snprintf(link, sizeof(link), "/sys/dev/block/%u:%u", major(partStat.st_rdev), minor(partStat.st_rdev));
    if (!realpath(link, sysPath)) return fail("resolve storage device", partition);
    char field[4200];
    unsigned long long number = 0, start = 0, sectors = 0;
    snprintf(field, sizeof(field), "%s/partition", sysPath);
    if (!readNumber(field, &number)) return fail("read partition number", partition);
    if (number != 2) { errno = EINVAL; return fail("unsupported partition number", partition); }
    snprintf(field, sizeof(field), "%s/start", sysPath);
    if (!readNumber(field, &start)) return fail("read partition start", partition);
    snprintf(field, sizeof(field), "%s/size", sysPath);
    if (!readNumber(field, &sectors)) return fail("read partition size", partition);
    char *slash = strrchr(sysPath, '/');
    if (!slash) { errno = EINVAL; return fail("invalid sysfs path", partition); }
    *slash = '\0';
    const char *diskName = strrchr(sysPath, '/');
    if (!diskName) { errno = EINVAL; return fail("invalid parent disk", partition); }
    char diskPath[4200];
    snprintf(diskPath, sizeof(diskPath), "/dev/%s", diskName + 1);
    Fd disk(open(diskPath, O_RDWR | O_CLOEXEC));
    if (disk.value < 0 || flock(disk.value, LOCK_EX | LOCK_NB) < 0) return fail("open/lock storage disk", diskPath);
    struct stat diskStat{};
    unsigned diskMajor = 0, diskMinor = 0;
    snprintf(field, sizeof(field), "%s/dev", sysPath);
    FILE *dev = fopen(field, "r");
    if (!dev) return fail("read parent disk identity", diskPath);
    const bool identityRead = fscanf(dev, "%u:%u", &diskMajor, &diskMinor) == 2;
    const int closed = fclose(dev);
    if (!identityRead || closed != 0 || fstat(disk.value, &diskStat) < 0 ||
        !S_ISBLK(diskStat.st_mode) || diskStat.st_rdev != makedev(diskMajor, diskMinor)) {
        errno = EINVAL;
        return fail("parent disk identity mismatch", diskPath);
    }
    int sectorSize = 0;
    uint64_t diskBytes = 0;
    if (ioctl(disk.value, BLKSSZGET, &sectorSize) < 0 ||
        ioctl(disk.value, BLKGETSIZE64, &diskBytes) < 0) return fail("read disk geometry", diskPath);
    if (sectorSize != 512 || diskBytes % 512) {
        errno = EINVAL;
        return fail("unsupported disk geometry", diskPath);
    }
    unsigned char super[1024];
    uint64_t currentBlocks = 0, targetBlocks = 0;
    if (pread(part.value, super, sizeof(super), 1024) != sizeof(super) ||
        !planFilesystem(super, sectors * 512, &currentBlocks, &targetBlocks)) {
        errno = EINVAL;
        return fail("storage is not a valid ext4 filesystem (left unchanged)", partition);
    }
    unsigned char mbr[512], original[512];
    if (pread(disk.value, mbr, sizeof(mbr), 0) != sizeof(mbr)) {
        errno = EIO;
        return fail("read partition table", diskPath);
    }
    memcpy(original, mbr, sizeof(mbr));
    uint32_t target = 0;
    if (!planPartition(mbr, diskBytes / 512, start, sectors, &target)) {
        errno = EINVAL;
        return fail("unsupported or inconsistent partition layout (left unchanged)", diskPath);
    }
    if (memcmp(original, mbr, sizeof(mbr)) != 0) {
        // Only the four-byte length changes; no partition is deleted or moved.
        const ssize_t written = pwrite(disk.value, mbr + 474, 4, 474);
        if (written != 4) {
            if (written >= 0) errno = EIO;
            return fail("write storage partition length", diskPath);
        }
        if (fsync(disk.value) < 0) {
            return fail("persist storage partition length", diskPath);
        }
    }
    if (sectors < target) {
        struct blkpg_partition change{};
        change.pno = 2;
        change.start = static_cast<long long>(start * 512);
        change.length = static_cast<long long>(uint64_t(target) * 512);
        struct blkpg_ioctl_arg argument{};
        argument.op = BLKPG_RESIZE_PARTITION;
        argument.datalen = sizeof(change);
        argument.data = &change;
        if (ioctl(disk.value, BLKPG, &argument) < 0) return fail("update kernel partition size", diskPath);
        uint64_t actual = 0;
        if (ioctl(part.value, BLKGETSIZE64, &actual) < 0 || actual != uint64_t(target) * 512) {
            errno = EIO;
            return fail("verify expanded partition", partition);
        }
        printf("[STORAGE] [INFO] expanded %s from %llu to %u sectors\n", partition, sectors, target);
    }
    return 0;
}

int growFilesystem(const char *partition, const char *mountPoint) {
    Fd part(open(partition, O_RDONLY | O_CLOEXEC));
    uint64_t bytes = 0;
    unsigned char super[1024];
    if (part.value < 0 || ioctl(part.value, BLKGETSIZE64, &bytes) < 0) return fail("read partition capacity", partition);
    if (pread(part.value, super, sizeof(super), 1024) != sizeof(super)) {
        errno = EIO;
        return fail("read ext4 superblock", partition);
    }
    uint64_t current = 0, target = 0;
    if (!planFilesystem(super, bytes, &current, &target)) {
        errno = EINVAL;
        return fail("invalid ext4 geometry (left unchanged)", partition);
    }
    if (current == target) return 0;
    Fd mounted(open(mountPoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    struct stat mountedStat{}, partStat{};
    if (mounted.value < 0 || fstat(mounted.value, &mountedStat) < 0 || fstat(part.value, &partStat) < 0)
        return fail("open mounted storage", mountPoint);
    if (!S_ISBLK(partStat.st_mode) || mountedStat.st_dev != partStat.st_rdev) {
        errno = EINVAL;
        return fail("mounted storage identity mismatch", mountPoint);
    }
    constexpr unsigned long resizeExt4 = _IOW('f', 16, uint64_t);
    printf("[STORAGE] [INFO] growing ext4 on %s from %llu to %llu blocks\n",
           partition, (unsigned long long)current, (unsigned long long)target);
    // Bound each resize to 1 GiB and report actual main-thread progress.
    const uint64_t step = (UINT64_C(1) << 30) / (uint64_t(1024) << read32(super + 24));
    while (current < target) {
        audioxWatchdogHeartbeat();
        uint64_t next = target - current > step ? current + step : target;
        if (ioctl(mounted.value, resizeExt4, &next) < 0 || fsync(mounted.value) < 0)
            return fail("grow ext4 filesystem", mountPoint);
        uint64_t verified = 0, capacity = 0;
        if (pread(part.value, super, sizeof(super), 1024) != sizeof(super) ||
            !planFilesystem(super, bytes, &verified, &capacity) || verified != next) {
            errno = EIO;
            return fail("verify expanded ext4 filesystem", partition);
        }
        current = verified;
    }
    printf("[STORAGE] [INFO] storage filesystem expanded successfully\n");
    return 0;
}
} // namespace audiox::storage
