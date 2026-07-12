#include "defs.hpp"
#include "system.hpp"

#include <errno.h>
#include <sys/syscall.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <unistd.h>
#include <fcntl.h>

namespace {

static int loadModule(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("[BOOT] [ERR] open module failed %s: %s\n", path, strerror(errno));
        return RET_ERR;
    }

    int rc = syscall(SYS_finit_module, fd, "", 0);
    int saved_errno = errno;
    close(fd);

    if (rc < 0 && saved_errno != EEXIST) {
        printf("[BOOT] [ERR] load module failed %s: %s (errno %d)\n", path, strerror(saved_errno), saved_errno);
        return RET_ERR;
    }
    return RET_OK;
}

static int loadModulesFromList(const char *path) {
    FILE *fp = fopen(path, "r");
    if (!fp) {
        printf("[BOOT] [ERR] open module list failed %s: %s\n", path, strerror(errno));
        return RET_ERR;
    }

    char line[768];
    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p = line;
        while (*p == ' ' || *p == '\t') {
            ++p;
        }
        if (*p == '\0' || *p == '\n' || *p == '#') {
            continue;
        }

        size_t len = strlen(p);
        while (len > 0 && (p[len - 1] == '\n' || p[len - 1] == '\r' || p[len - 1] == ' ' || p[len - 1] == '\t')) {
            p[len - 1] = '\0';
            --len;
        }
        if (len == 0) {
            continue;
        }

        printf("[BOOT] loading module %s\n", p);
        if (loadModule(p) != RET_OK) {
            fclose(fp);
            return RET_ERR;
        }
    }

    fclose(fp);
    return RET_OK;
}

static int waitForPath(const char *path, int attempts, useconds_t delay_us) {
    for (int i = 0; i < attempts; ++i) {
        if (access(path, F_OK) == 0) {
            return RET_OK;
        }
        usleep(delay_us);
    }
    return RET_ERR;
}

static int writeAllToFd(int fd, const void *buf, size_t len) {
    const char *bytes = (const char *)buf;
    size_t written = 0;
    while (written < len) {
        ssize_t nw = write(fd, bytes + written, len - written);
        if (nw < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nw == 0) {
            return -1;
        }
        written += (size_t)nw;
    }
    return 0;
}

static int copyFilePath(const char *src_path, const char *dst_path) {
    int src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        printf("[BOOT] [ERR] open src failed %s: %s\n", src_path, strerror(errno));
        return RET_ERR;
    }

    int dst_fd = open(dst_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dst_fd < 0) {
        printf("[BOOT] [ERR] open dst failed %s: %s\n", dst_path, strerror(errno));
        close(src_fd);
        return RET_ERR;
    }

    char buf[16384];
    while (1) {
        ssize_t nr = read(src_fd, buf, sizeof(buf));
        if (nr < 0) {
            if (errno == EINTR) {
                continue;
            }
            printf("[BOOT] [ERR] read failed %s: %s\n", src_path, strerror(errno));
            close(dst_fd);
            close(src_fd);
            return RET_ERR;
        }
        if (nr == 0) {
            break;
        }
        if (writeAllToFd(dst_fd, buf, (size_t)nr) < 0) {
            printf("[BOOT] [ERR] write failed %s: %s\n", dst_path, strerror(errno));
            close(dst_fd);
            close(src_fd);
            return RET_ERR;
        }
    }

    if (fsync(dst_fd) < 0) {
        printf("[BOOT] [ERR] fsync failed %s: %s\n", dst_path, strerror(errno));
        close(dst_fd);
        close(src_fd);
        return RET_ERR;
    }

    close(dst_fd);
    close(src_fd);
    return RET_OK;
}

static int mountBootloaderFilesystems() {
    if (ensureDir("/proc", 0555) != RET_OK) return RET_ERR;
    if (ensureDir("/sys", 0555) != RET_OK) return RET_ERR;
    if (ensureDir("/dev", 0755) != RET_OK) return RET_ERR;
    if (mountIfNeeded("proc", "/proc", "proc") != RET_OK) return RET_ERR;
    if (mountIfNeeded("sysfs", "/sys", "sysfs") != RET_OK) return RET_ERR;
    if (mountIfNeeded("devtmpfs", "/dev", "devtmpfs") != RET_OK) return RET_ERR;
    return RET_OK;
}

static int mountPartition(const char *device, const char *mount_point, const char *fs_type) {
    if (ensureDir("/mnt", 0755) != RET_OK) {
        return RET_ERR;
    }
    if (ensureDir(mount_point, 0755) != RET_OK) {
        return RET_ERR;
    }
    if (waitForPath(device, 20, 100000) != RET_OK) {
        printf("[BOOT] [ERR] device %s did not appear; likely missing block driver or module\n", device);
        return RET_ERR;
    }
    if (mount(device, mount_point, fs_type, MS_NOATIME, NULL) < 0) {
        printf("[BOOT] [ERR] mount %s on %s failed: %s\n", device, mount_point, strerror(errno));
        return RET_ERR;
    }
    return RET_OK;
}

static int loadBootModules() {
    return loadModulesFromList(BOOT_MODULE_LOAD_LIST_FILE);
}

static int applyStagedProgramUpdate() {
    if (mountPartition(CONFIG_DEVICE_PATH, ROOTFS_STAGING_MOUNT_POINT, "ext4") != RET_OK) {
        return RET_ERR;
    }

    char staged_path[256];
    int staged_n = snprintf(staged_path,
                            sizeof(staged_path),
                            "%s/%s/%s",
                            ROOTFS_STAGING_MOUNT_POINT,
                            STAGING_DIR_NAME,
                            PROGRAM_INITRAMFS_NAME);
    if (staged_n <= 0 || (size_t)staged_n >= sizeof(staged_path)) {
        (void)umount(ROOTFS_STAGING_MOUNT_POINT);
        return RET_ERR;
    }

    if (access(staged_path, F_OK) < 0) {
        (void)umount(ROOTFS_STAGING_MOUNT_POINT);
        return RET_WARN;
    }

    if (mountPartition(BOOT_DEVICE_PATH, BOOT_MOUNT_POINT, "vfat") != RET_OK) {
        (void)umount(ROOTFS_STAGING_MOUNT_POINT);
        return RET_ERR;
    }

    char tmp_path[256];
    char final_path[256];
    int tmp_n = snprintf(tmp_path, sizeof(tmp_path), "%s/%s.tmp", BOOT_MOUNT_POINT, PROGRAM_INITRAMFS_NAME);
    int final_n = snprintf(final_path, sizeof(final_path), "%s/%s", BOOT_MOUNT_POINT, PROGRAM_INITRAMFS_NAME);
    if (tmp_n <= 0 || final_n <= 0 || (size_t)tmp_n >= sizeof(tmp_path) || (size_t)final_n >= sizeof(final_path)) {
        (void)umount(BOOT_MOUNT_POINT);
        (void)umount(ROOTFS_STAGING_MOUNT_POINT);
        return RET_ERR;
    }

    unlink(tmp_path);
    if (copyFilePath(staged_path, tmp_path) != RET_OK) {
        unlink(tmp_path);
        (void)umount(BOOT_MOUNT_POINT);
        (void)umount(ROOTFS_STAGING_MOUNT_POINT);
        return RET_ERR;
    }

    if (unlink(final_path) < 0 && errno != ENOENT) {
        printf("[BOOT] [ERR] unlink old runtime failed: %s\n", strerror(errno));
        unlink(tmp_path);
        (void)umount(BOOT_MOUNT_POINT);
        (void)umount(ROOTFS_STAGING_MOUNT_POINT);
        return RET_ERR;
    }

    if (rename(tmp_path, final_path) < 0) {
        printf("[BOOT] [ERR] rename runtime failed: %s\n", strerror(errno));
        unlink(tmp_path);
        (void)umount(BOOT_MOUNT_POINT);
        (void)umount(ROOTFS_STAGING_MOUNT_POINT);
        return RET_ERR;
    }

    if (unlink(staged_path) < 0) {
        printf("[BOOT] [WARN] staged runtime cleanup failed: %s\n", strerror(errno));
    }

    sync();
    (void)umount(BOOT_MOUNT_POINT);
    (void)umount(ROOTFS_STAGING_MOUNT_POINT);
    printf("[BOOT] runtime update installed, rebooting into new image\n");
    sync();
    if (reboot(RB_AUTOBOOT) < 0) {
        printf("[BOOT] [ERR] reboot failed: %s\n", strerror(errno));
        return RET_ERR;
    }
    return RET_OK;
}

static int bootProgramRuntime() {
    char runtime_init[256];
    int n = snprintf(runtime_init, sizeof(runtime_init), "%s/init", PROGRAM_RUNTIME_ROOT);
    if (n <= 0 || (size_t)n >= sizeof(runtime_init)) {
        return RET_ERR;
    }

    if (access(runtime_init, X_OK) < 0) {
        printf("[BOOT] [ERR] runtime init missing at %s: %s\n", runtime_init, strerror(errno));
        return RET_ERR;
    }

    if (chdir(PROGRAM_RUNTIME_ROOT) < 0) {
        printf("[BOOT] [ERR] chdir %s failed: %s\n", PROGRAM_RUNTIME_ROOT, strerror(errno));
        return RET_ERR;
    }
    if (chroot(".") < 0) {
        printf("[BOOT] [ERR] chroot %s failed: %s\n", PROGRAM_RUNTIME_ROOT, strerror(errno));
        return RET_ERR;
    }
    if (chdir("/") < 0) {
        printf("[BOOT] [ERR] chdir / failed after chroot: %s\n", strerror(errno));
        return RET_ERR;
    }

    char *const argv[] = {(char *)"/init", NULL};
    execv("/init", argv);
    printf("[BOOT] [ERR] exec /init failed: %s\n", strerror(errno));
    return RET_ERR;
}

} // namespace

int main() {
    umask(0);
    printf("[BOOT] audiox bootloader start\n");

    if (mountBootloaderFilesystems() != RET_OK) {
        printf("[BOOT] [CRIT] failed to mount base filesystems\n");
        while (1) {
            sleep(60);
        }
    }

    if (loadBootModules() != RET_OK) {
        printf("[BOOT] [CRIT] failed to load boot storage modules\n");
        while (1) {
            sleep(60);
        }
    }

    int update_rc = applyStagedProgramUpdate();
    if (update_rc == RET_ERR) {
        printf("[BOOT] [CRIT] staged update failed\n");
        while (1) {
            sleep(60);
        }
    }

    if (bootProgramRuntime() != RET_OK) {
        printf("[BOOT] [CRIT] failed to enter runtime\n");
        while (1) {
            sleep(60);
        }
    }

    return 0;
}