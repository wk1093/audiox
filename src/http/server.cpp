#include "http/context.hpp"
#include "http/ffmpeg.hpp"
#include "audio/context.hpp"
#include "system.hpp"
#include "init.hpp"
#include "update_archive.hpp"
#include "config/backup.hpp"
#include "watchdog_heartbeat.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/reboot.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#define HTTP_REQ_MAX 65536
#define HTTP_BODY_MAX 49152
#define HTTP_INITRAM_PATH "/api/initram"
#define HTTP_INITRAM_MAX (64u * 1024u * 1024u)
#define HTTP_BOOT_LOADER_PATH "/api/boot/loader"
#define HTTP_BOOT_LOADER_FILE "/boot/initramfs.cpio.gz"
#define HTTP_BOOT_KERNEL_PATH "/api/boot/kernel"
#define HTTP_BOOT_DIR "/boot"
#define HTTP_BOOT_KERNEL_TEMP_FILE HTTP_BOOT_DIR "/audiox-kernel.update"
#define HTTP_BOOT_UPLOAD_MAX (150u * 1024u * 1024u)
#define HTTP_BOOT_CONFIG_FILE "/boot/config.txt"
#define HTTP_BOOT_CMDLINE_FILE "/boot/cmdline.txt"
#define HTTP_UPDATE_PATH "/api/update"
#define HTTP_UPDATE_MAX (256u * 1024u * 1024u)
#define HTTP_INITRAM_RCV_TIMEOUT_SEC 10
#define HTTP_ROOTFS_PREFIX "/api/rootfs/"
#define HTTP_ROOTFS_UPLOAD_MAX (150u * 1024u * 1024u)

#define HTTP_API_PREFIX "/api/"
#define HTTP_DEFAULT_HTML_PATH "/etc/www/index.html"

#define HTTP_STAGE_DIR ROOT_MOUNT_POINT "/staging"
#define HTTP_STAGE_INITRAMFS_PATH PROGRAM_STAGED_PATH
#define HTTP_STAGE_INITRAMFS_PART_PATH PROGRAM_STAGED_PATH ".part"
#define HTTP_STAGE_UPDATE_PATH HTTP_STAGE_DIR "/update.axu.part"

namespace {

static int setSocketTimeouts(int fd, long recv_ms, long send_ms) {
    if (fd < 0) {
        return -1;
    }

    struct timeval recv_to;
    recv_to.tv_sec = recv_ms / 1000;
    recv_to.tv_usec = (recv_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_to, sizeof(recv_to)) < 0) {
        return -1;
    }

    struct timeval send_to;
    send_to.tv_sec = send_ms / 1000;
    send_to.tv_usec = (send_ms % 1000) * 1000;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_to, sizeof(send_to)) < 0) {
        return -1;
    }

    return 0;
}

static int sendErrnoResponse(HttpServer *server,
                             int cfd,
                             const char *status,
                             const char *prefix,
                             int saved_errno) {
    char body[256];
    int n = snprintf(body,
                     sizeof(body),
                     "%s: %s\n",
                     prefix ? prefix : "request failed",
                     strerror(saved_errno));
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n >= sizeof(body)) {
        n = (int)(sizeof(body) - 1);
    }
    return server->sendResponse(cfd,
                                status ? status : "500 Internal Server Error",
                                "text/plain; charset=utf-8",
                                body,
                                (size_t)n);
}

static int checkStageCapacity(HttpServer *server, int cfd, size_t content_length) {
    struct statvfs st;
    if (statvfs(ROOT_MOUNT_POINT, &st) < 0) {
        return 0;
    }

    unsigned long long free_bytes = (unsigned long long)st.f_bavail * (unsigned long long)st.f_frsize;
    if (free_bytes >= (unsigned long long)content_length) {
        return 0;
    }

    char body[256];
    int n = snprintf(body,
                     sizeof(body),
                     "insufficient storage: need %lu bytes, have %llu bytes free on %s\n",
                     (unsigned long)content_length,
                     free_bytes,
                     ROOT_MOUNT_POINT);
    if (n < 0) {
        n = 0;
    }
    if ((size_t)n >= sizeof(body)) {
        n = (int)(sizeof(body) - 1);
    }
    (void)server->sendResponse(cfd,
                               "507 Insufficient Storage",
                               "text/plain; charset=utf-8",
                               body,
                               (size_t)n);
    return -1;
}

static int writeAllToFd(int fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    size_t written = 0;
    while (written < len) {
        audioxWatchdogHeartbeat();
        ssize_t nw = write(fd, p + written, len - written);
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

static int streamRequestBodyToFd(int cfd,
                                 int fd,
                                 const char *initial_body,
                                 size_t initial_body_len,
                                 size_t content_length) {
    size_t written = 0;
    size_t initial_write = initial_body_len;
    if (initial_write > content_length) {
        initial_write = content_length;
    }
    if (initial_write > 0 && writeAllToFd(fd, initial_body, initial_write) < 0) {
        return -1;
    }
    written = initial_write;

    char buf[16384];
    while (written < content_length) {
        audioxWatchdogHeartbeat();
        size_t want = content_length - written;
        if (want > sizeof(buf)) {
            want = sizeof(buf);
        }

        ssize_t nr = recv(cfd, buf, want, 0);
        if (nr < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (nr == 0) {
            return 1;
        }
        if (writeAllToFd(fd, buf, (size_t)nr) < 0) {
            return -1;
        }
        written += (size_t)nr;
    }
    return 0;
}

static int mountBootPartition(void) {
    if (mkdir("/boot", 0755) < 0 && errno != EEXIST) {
        return -1;
    }
    if (mount(BOOT_DEVICE_PATH, "/boot", "vfat", MS_NOATIME, NULL) < 0 && errno != EBUSY) {
        return -1;
    }
    return 0;
}

// Kernel file the installed config.txt boots; the boot partition must already be mounted.
static void currentBootKernelName(char *out, size_t out_sz) {
    char text[16384];
    size_t len = 0;
    int fd = open(HTTP_BOOT_CONFIG_FILE, O_RDONLY);
    if (fd >= 0) {
        ssize_t nr = read(fd, text, sizeof(text));
        if (nr > 0) {
            len = (size_t)nr;
        }
        close(fd);
    }
    audioxConfigKernelName(text, len, out, out_sz);
}

// Board family of the running hardware from the device-tree SoC, falling back to the build target.
static const char *detectBoard(void) {
    char compat[512];
    int fd = open("/proc/device-tree/compatible", O_RDONLY);
    if (fd >= 0) {
        ssize_t nr = read(fd, compat, sizeof(compat) - 1);
        close(fd);
        if (nr > 0) {
            for (ssize_t i = 0; i < nr; ++i) {
                if (compat[i] == '\0') {
                    compat[i] = '\n';
                }
            }
            compat[nr] = '\0';
            if (strstr(compat, "brcm,bcm2712")) {
                return "pi5";
            }
            if (strstr(compat, "brcm,bcm2711")) {
                return "pi4";
            }
        }
    }
#if defined(AUDIOX_BOARD)
    return AUDIOX_BOARD;
#else
    return "";
#endif
}

// Extracts a validated kernel file name from a "name=" query parameter.
static bool queryKernelName(const char *query, char *out, size_t out_sz) {
    if (!query) {
        return false;
    }
    const char *p = query;
    while (p && *p) {
        if (strncmp(p, "name=", 5) == 0) {
            const char *v = p + 5;
            size_t len = strcspn(v, "&");
            if (len >= out_sz) {
                return false;
            }
            memcpy(out, v, len);
            out[len] = '\0';
            return true;
        }
        p = strchr(p, '&');
        if (p) {
            ++p;
        }
    }
    return false;
}

static int handleBootFile(HttpServer *server,
                          int cfd,
                          const char *method,
                          const char *path,
                          const char *query,
                          size_t content_length,
                          const char *initial_body,
                          size_t initial_body_len) {
    char kernel_path[sizeof(HTTP_BOOT_DIR) + AUDIOX_UPDATE_VALUE_MAX + 1];
    const char *file_path = NULL;
    const bool is_kernel = strcmp(path, HTTP_BOOT_KERNEL_PATH) == 0;
    if (strcmp(path, HTTP_BOOT_LOADER_PATH) == 0) {
        file_path = HTTP_BOOT_LOADER_FILE;
    } else if (!is_kernel) {
        return -1;
    }

    if (strcmp(method, "GET") != 0 && strcmp(method, "PUT") != 0) {
        static const char mna[] = "method not allowed\n";
        return server->sendResponse(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
    }
    if (mountBootPartition() < 0) {
        static const char err[] = "boot partition unavailable\n";
        return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    if (is_kernel) {
        // ?name= selects the kernel file; otherwise the kernel the current config.txt boots.
        char name[AUDIOX_UPDATE_VALUE_MAX];
        if (query && strstr(query, "name=")) {
            if (!queryKernelName(query, name, sizeof(name)) || !audioxUpdateValidKernelName(name)) {
                static const char bad[] = "invalid kernel name (expected kernel*.img)\n";
                return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
            }
        } else {
            currentBootKernelName(name, sizeof(name));
        }
        snprintf(kernel_path, sizeof(kernel_path), "%s/%s", HTTP_BOOT_DIR, name);
        file_path = kernel_path;
    }

    if (strcmp(method, "GET") == 0) {
        if (access(file_path, R_OK) < 0) {
            static const char err[] = "boot file unavailable\n";
            return server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        }
        return server->sendFilePath(cfd, file_path, "application/octet-stream");
    }

    if (content_length == 0) {
        static const char bad[] = "missing body\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }
    if (content_length > HTTP_BOOT_UPLOAD_MAX) {
        static const char too_large[] = "payload too large\n";
        return server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
    }

    char temp_path[sizeof(kernel_path) + sizeof(".part")];
    int path_len = snprintf(temp_path, sizeof(temp_path), "%s.part", file_path);
    if (path_len <= 0 || (size_t)path_len >= sizeof(temp_path)) {
        static const char err[] = "boot file path too long\n";
        return server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    int fd = open(temp_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload open failed", errno);
    }

    int stream_rc = streamRequestBodyToFd(cfd, fd, initial_body, initial_body_len, content_length);
    if (stream_rc < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload failed", saved_errno);
    }
    if (stream_rc > 0) {
        close(fd);
        unlink(temp_path);
        static const char err[] = "incomplete upload\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }
    if (fsync(fd) < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload sync failed", saved_errno);
    }
    if (close(fd) < 0) {
        int saved_errno = errno;
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload close failed", saved_errno);
    }
    if (prepareBootUpdateRecovery() != RET_OK) {
        int saved_errno = errno;
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot backup failed", saved_errno);
    }
    if (rename(temp_path, file_path) < 0) {
        int saved_errno = errno;
        unlink(temp_path);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot upload finalize failed", saved_errno);
    }

    static const char ok[] = "{\"ok\":true}\n";
    return server->sendResponse(cfd, "200 OK", "application/json; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleInitramUpload(HttpServer *server,
                               int cfd,
                               const char *initial_body,
                               size_t initial_body_len,
                               size_t content_length) {
    if (!server) {
        return -1;
    }

    if (content_length == 0) {
        static const char bad[] = "missing body\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    if (checkStageCapacity(server, cfd, content_length) < 0) {
        return 0;
    }

    if (ensureDir(HTTP_STAGE_DIR, 0755) != RET_OK) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging mkdir failed", errno);
    }

    int fd = open(HTTP_STAGE_INITRAMFS_PART_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging open failed", errno);
    }

    int stream_rc = streamRequestBodyToFd(cfd, fd, initial_body, initial_body_len, content_length);
    if (stream_rc < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging write failed", saved_errno);
    }
    if (stream_rc > 0) {
        close(fd);
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        static const char err[] = "incomplete upload\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    if (fsync(fd) < 0) {
        int saved_errno = errno;
        close(fd);
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging fsync failed", saved_errno);
    }

    close(fd);

    if (prepareBootUpdateRecovery() != RET_OK) {
        int saved_errno = errno;
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot backup failed", saved_errno);
    }

    if (rename(HTTP_STAGE_INITRAMFS_PART_PATH, HTTP_STAGE_INITRAMFS_PATH) < 0) {
        int saved_errno = errno;
        unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging finalize failed", saved_errno);
    }

    static const char ok[] = "ok staged rebooting\n";
    (void)server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
    sync();
    usleep(200000);
    if (reboot(RB_AUTOBOOT) < 0) {
        printf("[INIT] [ERR] HTTP initram reboot failed: %s\n", strerror(errno));
    }
    return 0;
}

typedef AudioxUpdateEntry UpdateEntry;

static int readUpdateEntryText(int fd, const UpdateEntry &entry, char *buf, size_t buf_sz) {
    if (entry.size >= buf_sz) {
        errno = EFBIG;
        return -1;
    }
    if (audioxUpdateReadAt(fd, buf, (size_t)entry.size, entry.offset) < 0) {
        return -1;
    }
    buf[entry.size] = '\0';
    return 0;
}

// Reads the metadata (v1 archives default to kernel8.img with no board), rejects archives built
// for another board, and requires the archive's config.txt to boot the archive's kernel.
static const char *checkUpdateMetadata(int fd, const UpdateEntry entries[AUDIOX_UPDATE_KNOWN_TYPES], AudioxUpdateMetadata *meta) {
    const UpdateEntry &meta_entry = entries[AUDIOX_UPDATE_METADATA - 1u];
    if (meta_entry.size == 0) {
        memset(meta, 0, sizeof(*meta));
        snprintf(meta->kernel, sizeof(meta->kernel), "%s", AUDIOX_UPDATE_DEFAULT_KERNEL);
    } else {
        char text[AUDIOX_UPDATE_METADATA_MAX + 1];
        if (readUpdateEntryText(fd, meta_entry, text, sizeof(text)) < 0) {
            return "failed to read update metadata\n";
        }
        if (audioxUpdateParseMetadata(text, (size_t)meta_entry.size, meta) != NULL) {
            return "invalid update metadata\n";
        }
        const char *board = detectBoard();
        if (board[0] && strcmp(board, meta->board) != 0) {
            static char msg[128];
            snprintf(msg, sizeof(msg), "update archive is for %s but this device is %s\n", meta->board, board);
            return msg;
        }
    }

    const UpdateEntry &config_entry = entries[AUDIOX_UPDATE_CONFIG - 1u];
    char *config_text = (char *)malloc((size_t)config_entry.size + 1u);
    if (!config_text) {
        return "out of memory reading boot config\n";
    }
    if (readUpdateEntryText(fd, config_entry, config_text, (size_t)config_entry.size + 1u) < 0) {
        free(config_text);
        return "failed to read boot config entry\n";
    }
    char config_kernel[AUDIOX_UPDATE_VALUE_MAX];
    audioxConfigKernelName(config_text, (size_t)config_entry.size, config_kernel, sizeof(config_kernel));
    free(config_text);
    if (strcmp(config_kernel, meta->kernel) != 0) {
        return "update archive config.txt does not boot the archive kernel\n";
    }

    const UpdateEntry &cmdline_entry = entries[AUDIOX_UPDATE_CMDLINE - 1u];
    if (cmdline_entry.size != 0) {
        char cmdline[AUDIOX_UPDATE_CMDLINE_MAX + 1];
        if (readUpdateEntryText(fd, cmdline_entry, cmdline, sizeof(cmdline)) < 0) {
            return "failed to read cmdline entry\n";
        }
        if (audioxValidateCmdline(cmdline, (size_t)cmdline_entry.size) != NULL) {
            return "update archive cmdline.txt is invalid (must be one line containing rdinit=/init)\n";
        }
    }
    return NULL;
}

static int writeUpdateEntry(int archive_fd, const UpdateEntry &entry, const char *dest_path) {
    int out_fd = open(dest_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out_fd < 0) {
        return -1;
    }

    unsigned char buf[16384];
    uint64_t done = 0;
    while (done < entry.size) {
        size_t want = sizeof(buf);
        if (entry.size - done < want) {
            want = (size_t)(entry.size - done);
        }
        if (audioxUpdateReadAt(archive_fd, buf, want, entry.offset + (off_t)done) < 0 ||
            writeAllToFd(out_fd, buf, want) < 0) {
            int saved_errno = errno;
            close(out_fd);
            unlink(dest_path);
            errno = saved_errno;
            return -1;
        }
        done += want;
    }

    if (fsync(out_fd) < 0) {
        int saved_errno = errno;
        close(out_fd);
        unlink(dest_path);
        errno = saved_errno;
        return -1;
    }
    if (close(out_fd) < 0) {
        int saved_errno = errno;
        unlink(dest_path);
        errno = saved_errno;
        return -1;
    }
    return 0;
}

static void cleanupUpdateFiles(void) {
    unlink(HTTP_STAGE_UPDATE_PATH);
    unlink(HTTP_STAGE_INITRAMFS_PART_PATH);
    unlink(HTTP_BOOT_CONFIG_FILE ".update");
    unlink(HTTP_BOOT_CMDLINE_FILE ".update");
    unlink(HTTP_BOOT_LOADER_FILE ".update");
    unlink(HTTP_BOOT_KERNEL_TEMP_FILE);
}

static int handleUpdateUpload(HttpServer *server,
                              int cfd,
                              const char *initial_body,
                              size_t initial_body_len,
                              size_t content_length) {
    if (content_length == 0) {
        static const char bad[] = "missing body\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    // The archive and its extracted runtime are both temporarily stored in staging.
    if (checkStageCapacity(server, cfd, content_length * 2u) < 0) {
        return 0;
    }
    if (ensureDir(HTTP_STAGE_DIR, 0755) != RET_OK) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "staging mkdir failed", errno);
    }

    cleanupUpdateFiles();
    int archive_fd = open(HTTP_STAGE_UPDATE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (archive_fd < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "update archive open failed", errno);
    }

    int stream_rc = streamRequestBodyToFd(cfd, archive_fd, initial_body, initial_body_len, content_length);
    if (stream_rc < 0) {
        int saved_errno = errno;
        close(archive_fd);
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "update archive write failed", saved_errno);
    }
    if (stream_rc > 0) {
        close(archive_fd);
        cleanupUpdateFiles();
        static const char err[] = "incomplete upload\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }
    if (fsync(archive_fd) < 0) {
        int saved_errno = errno;
        close(archive_fd);
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "update archive sync failed", saved_errno);
    }

    UpdateEntry entries[AUDIOX_UPDATE_KNOWN_TYPES];
    memset(entries, 0, sizeof(entries));
    const char *parse_error = audioxUpdateParseArchive(archive_fd, content_length, entries);
    if (parse_error) {
        close(archive_fd);
        cleanupUpdateFiles();
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", parse_error, strlen(parse_error));
    }
    AudioxUpdateMetadata meta;
    const char *meta_error = checkUpdateMetadata(archive_fd, entries, &meta);
    if (meta_error) {
        close(archive_fd);
        cleanupUpdateFiles();
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", meta_error, strlen(meta_error));
    }
    printf("[HTTP] update archive: board=%s kernel=%s version=%s kernel_version=%s\n",
           meta.board[0] ? meta.board : "unspecified", meta.kernel,
           meta.version[0] ? meta.version : "unspecified",
           meta.kernel_version[0] ? meta.kernel_version : "unspecified");
    if (mountBootPartition() < 0) {
        close(archive_fd);
        cleanupUpdateFiles();
        static const char err[] = "boot partition unavailable\n";
        return server->sendResponse(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    char kernel_path[sizeof(HTTP_BOOT_DIR) + AUDIOX_UPDATE_VALUE_MAX + 1];
    snprintf(kernel_path, sizeof(kernel_path), "%s/%s", HTTP_BOOT_DIR, meta.kernel);

    struct UpdateTarget {
        uint32_t type;
        const char *temp_path;
        const char *final_path;
    };
    // config.txt goes last: it selects the kernel, so it only switches once that kernel exists.
    // The optional cmdline entry is skipped when absent so the device keeps its current one.
    const UpdateTarget boot_targets[] = {
        {AUDIOX_UPDATE_KERNEL, HTTP_BOOT_KERNEL_TEMP_FILE, kernel_path},
        {AUDIOX_UPDATE_BOOTLOADER, HTTP_BOOT_LOADER_FILE ".update", HTTP_BOOT_LOADER_FILE},
        {AUDIOX_UPDATE_CMDLINE, HTTP_BOOT_CMDLINE_FILE ".update", HTTP_BOOT_CMDLINE_FILE},
        {AUDIOX_UPDATE_CONFIG, HTTP_BOOT_CONFIG_FILE ".update", HTTP_BOOT_CONFIG_FILE},
    };

    if (writeUpdateEntry(archive_fd, entries[AUDIOX_UPDATE_PROGRAM - 1u], HTTP_STAGE_INITRAMFS_PART_PATH) < 0) {
        int saved_errno = errno;
        close(archive_fd);
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "runtime extract failed", saved_errno);
    }
    for (const UpdateTarget &target : boot_targets) {
        if (entries[target.type - 1u].size == 0) {
            continue;
        }
        if (writeUpdateEntry(archive_fd, entries[target.type - 1u], target.temp_path) < 0) {
            int saved_errno = errno;
            close(archive_fd);
            cleanupUpdateFiles();
            return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot file extract failed", saved_errno);
        }
    }
    close(archive_fd);

    if (prepareBootUpdateRecovery() != RET_OK) {
        int saved_errno = errno;
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot backup failed", saved_errno);
    }

    for (const UpdateTarget &target : boot_targets) {
        if (entries[target.type - 1u].size == 0) {
            continue;
        }
        if (rename(target.temp_path, target.final_path) < 0) {
            int saved_errno = errno;
            cleanupUpdateFiles();
            return sendErrnoResponse(server, cfd, "500 Internal Server Error", "boot file install failed", saved_errno);
        }
    }
    sync();

    if (rename(HTTP_STAGE_INITRAMFS_PART_PATH, HTTP_STAGE_INITRAMFS_PATH) < 0) {
        int saved_errno = errno;
        cleanupUpdateFiles();
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "runtime staging failed", saved_errno);
    }
    unlink(HTTP_STAGE_UPDATE_PATH);

    static const char ok[] = "ok update installed rebooting\n";
    (void)server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
    sync();
    usleep(200000);
    if (reboot(RB_AUTOBOOT) < 0) {
        printf("[INIT] [ERR] HTTP update reboot failed: %s\n", strerror(errno));
    }
    return 0;
}

static int safeApiSuffix(const char *suffix) {
    if (!suffix) {
        return 0;
    }
    if (!suffix[0]) {
        return 1;
    }
    if (strstr(suffix, "..") != NULL) {
        return 0;
    }
    if (strchr(suffix, '\\') != NULL) {
        return 0;
    }
    return 1;
}

static int ensureParentDirs(const char *path) {
    if (!path || !path[0]) {
        return -1;
    }

    char tmp[512];
    size_t n = strnlen(path, sizeof(tmp) - 1);
    if (n == 0 || n >= sizeof(tmp) - 1) {
        return -1;
    }
    memcpy(tmp, path, n);
    tmp[n] = '\0';

    for (char *p = tmp + 1; *p; ++p) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        if (mkdir(tmp, 0755) < 0 && errno != EEXIST) {
            return -1;
        }
        *p = '/';
    }

    return 0;
}

static int isSfxUploadSuffix(const char *suffix) {
    return (suffix && strncmp(suffix, "sfx/", 4) == 0 && suffix[4] != '\0') ? 1 : 0;
}

static int buildSfxFinalWavPath(const char *inputPath, char *out, size_t outSz) {
    if (!inputPath || !out || outSz == 0) {
        return -1;
    }

    size_t n = strnlen(inputPath, outSz - 1);
    if (n == 0 || n >= outSz - 1) {
        return -1;
    }
    memcpy(out, inputPath, n);
    out[n] = '\0';

    char *slash = strrchr(out, '/');
    char *leaf = slash ? slash + 1 : out;
    if (!leaf[0]) {
        return -1;
    }

    char *dot = strrchr(leaf, '.');
    if (dot) {
        *dot = '\0';
    }

    size_t stemLen = strlen(out);
    if (stemLen + 4 >= outSz) {
        return -1;
    }
    memcpy(out + stemLen, ".wav", 5);
    return 0;
}

static int runFfmpegTranscodeToWav(const char *inputPath, const char *outputPath) {
    if (!inputPath || !outputPath || !inputPath[0] || !outputPath[0]) {
        return -1;
    }

    const char *ffmpeg_path = getFfmpegPath();
    if (!ffmpeg_path) {
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        int devNull = open("/dev/null", O_WRONLY);
        if (devNull >= 0) {
            (void)dup2(devNull, STDOUT_FILENO);
            (void)dup2(devNull, STDERR_FILENO);
            if (devNull > STDERR_FILENO) {
                close(devNull);
            }
        }

        execl(ffmpeg_path,
              "ffmpeg",
              "-hide_banner",
              "-loglevel", "error",
              "-y",
              "-i", inputPath,
              "-vn",
              "-acodec", "pcm_s16le",
              "-ar", "48000",
              "-ac", "2",
              "-f", "wav",
              outputPath,
              (char *)NULL);
        
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    if (!WIFEXITED(status)) {
        return -1;
    }
    if (WEXITSTATUS(status) != 0) {
        return -1;
    }

    return 0;
}

static int maybePreprocessSfxUpload(HttpServer *server,
                                    int cfd,
                                    const char *suffix,
                                    const char *uploadedPath,
                                    char *finalPath,
                                    size_t finalPathSz) {
    if (!uploadedPath || !uploadedPath[0] || !finalPath || finalPathSz == 0) {
        return -1;
    }

    size_t n = strnlen(uploadedPath, finalPathSz - 1);
    memcpy(finalPath, uploadedPath, n);
    finalPath[n] = '\0';

    if (!isSfxUploadSuffix(suffix)) {
        return 0;
    }

    char wavPath[512];
    if (buildSfxFinalWavPath(uploadedPath, wavPath, sizeof(wavPath)) < 0) {
        static const char bad[] = "bad sfx path\n";
        (void)unlink(uploadedPath);
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return -1;
    }

    char tempWavPath[512];
    int tn = snprintf(tempWavPath, sizeof(tempWavPath), "%s.fftmp", wavPath);
    if (tn <= 0 || (size_t)tn >= sizeof(tempWavPath)) {
        static const char bad[] = "sfx path too long\n";
        (void)unlink(uploadedPath);
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return -1;
    }

    if (runFfmpegTranscodeToWav(uploadedPath, tempWavPath) != 0) {
        (void)unlink(tempWavPath);
        (void)unlink(uploadedPath);
        static const char err[] = "ffmpeg conversion failed\n";
        (void)server->sendResponse(cfd, "415 Unsupported Media Type", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return -1;
    }

    if (rename(tempWavPath, wavPath) < 0) {
        int saved = errno;
        (void)unlink(tempWavPath);
        (void)unlink(uploadedPath);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "sfx finalize failed", saved);
    }

    if (strcmp(uploadedPath, wavPath) != 0) {
        (void)unlink(uploadedPath);
    }

    if (server && server->app && server->app->audio) {
        int reloadRc = server->app->audio->reloadSfxBank();
        if (reloadRc == RET_ERR) {
            printf("[INIT] [WARN] soundboard bank reload failed after ffmpeg preprocess\n");
        }
    }

    size_t wn = strnlen(wavPath, finalPathSz - 1);
    memcpy(finalPath, wavPath, wn);
    finalPath[wn] = '\0';
    return 1;
}

static int handleRootfsUpload(HttpServer *server,
                              int cfd,
                              const char *path,
                              const char *initial_body,
                              size_t initial_body_len,
                              size_t content_length) {
    if (!server || !path || strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX)) != 0) {
        return -1;
    }

    const char *suffix = path + strlen(HTTP_ROOTFS_PREFIX);
    if (!safeApiSuffix(suffix) || !suffix[0] || suffix[strlen(suffix) - 1] == '/') {
        static const char bad[] = "bad file path\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    if (content_length == 0) {
        static const char bad[] = "missing body\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    if (content_length > HTTP_ROOTFS_UPLOAD_MAX) {
        static const char too_large[] = "payload too large\n";
        return server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
    }

    char fs_path[512];
    int pn = snprintf(fs_path, sizeof(fs_path), "%s%s", ROOT_MOUNT_POINT "/", suffix);
    if (pn <= 0 || (size_t)pn >= sizeof(fs_path)) {
        static const char bad[] = "path too long\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
    }

    if (ensureParentDirs(fs_path) < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "mkdir failed", errno);
    }

    int fd = open(fs_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "open failed", errno);
    }

    int stream_rc = streamRequestBodyToFd(cfd, fd, initial_body, initial_body_len, content_length);
    if (stream_rc < 0) {
        int saved_errno = errno;
        close(fd);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "write failed", saved_errno);
    }
    if (stream_rc > 0) {
        close(fd);
        static const char err[] = "incomplete upload\n";
        return server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    }

    if (fsync(fd) < 0) {
        int saved_errno = errno;
        close(fd);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "fsync failed", saved_errno);
    }
    close(fd);

    // Auto-set executable permissions for common binary paths
    const char *leaf = strrchr(suffix, '/');
    leaf = leaf ? (leaf + 1) : suffix;
    if (strncmp(leaf, "ffmpeg", 6) == 0 ||
        strstr(suffix, "/bin/") != NULL ||
        strstr(suffix, "/sbin/") != NULL) {
        chmod(fs_path, 0755);
    }

    char finalPath[512];
    int preprocessRc = maybePreprocessSfxUpload(server,
                                                cfd,
                                                suffix,
                                                fs_path,
                                                finalPath,
                                                sizeof(finalPath));
    if (preprocessRc < 0) {
        return 0;
    }

    if (preprocessRc > 0) {
        const char *leaf = strrchr(finalPath, '/');
        leaf = leaf ? (leaf + 1) : finalPath;
        char okBody[256];
        int on = snprintf(okBody, sizeof(okBody), "ok converted %s\n", leaf);
        if (on < 0) {
            on = 0;
        }
        if ((size_t)on >= sizeof(okBody)) {
            on = (int)(sizeof(okBody) - 1);
        }
        return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", okBody, (size_t)on);
    }

    static const char ok[] = "ok\n";
    return server->sendResponse(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
}

static int handleConfigBackup(HttpServer *server, int cfd, const char *method,
                              const char *path, const char *initial, size_t initialLen,
                              size_t contentLength) {
    bool restoring = strcmp(path, "/api/config/restore") == 0;
    if (strcmp(method, restoring ? "PUT" : "GET")) {
        static const char message[] = "method not allowed\n";
        return server->sendResponse(cfd, "405 Method Not Allowed", "text/plain", message, sizeof(message) - 1);
    }
    if (restoring && (contentLength < 20 || contentLength > audiox::backup::maxArchiveSize)) {
        static const char message[] = "backup must be between 20 bytes and 4 GiB\n";
        return server->sendResponse(cfd, "413 Payload Too Large", "text/plain", message, sizeof(message) - 1);
    }
    if (restoring) {
        struct statvfs storage;
        if (statvfs(ROOT_MOUNT_POINT, &storage) < 0) {
            return sendErrnoResponse(server, cfd, "500 Internal Server Error", "check restore storage", errno);
        }
        uint64_t freeBytes = (uint64_t)storage.f_bavail * storage.f_frsize;
        if (freeBytes < (uint64_t)contentLength * 2 + 16 * 1024 * 1024) {
            static const char message[] = "insufficient storage for backup upload, extracted files and restore metadata; no settings changed\n";
            return server->sendResponse(cfd, "507 Insufficient Storage", "text/plain", message, sizeof(message) - 1);
        }
    }
    char temp[] = ROOT_MOUNT_POINT "/.backup-transfer-XXXXXX";
    int fd = mkstemp(temp);
    if (fd < 0) return sendErrnoResponse(server, cfd, "500 Internal Server Error", "create backup transfer", errno);
    if (unlink(temp) < 0) {
        int saved = errno;
        close(fd);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "unlink backup transfer", saved);
    }
    char error[256] = {};
    int rc;
    if (restoring) {
        rc = streamRequestBodyToFd(cfd, fd, initial, initialLen, contentLength);
        if (rc) {
            int saved = errno;
            close(fd);
            if (rc < 0) return sendErrnoResponse(server, cfd, "500 Internal Server Error", "receive backup", saved);
            static const char message[] = "incomplete backup upload; no settings changed\n";
            return server->sendResponse(cfd, "400 Bad Request", "text/plain", message, sizeof(message) - 1);
        }
        rc = audiox::backup::stage(ROOT_MOUNT_POINT, fd, contentLength, error, sizeof(error));
    } else {
        rc = audiox::backup::create(ROOT_MOUNT_POINT, fd, error, sizeof(error));
    }
    if (rc < 0) {
        close(fd);
        printf("[CONFIG] [ERR] %s failed: %s\n", restoring ? "restore" : "backup", error);
        return server->sendResponse(cfd, restoring ? "400 Bad Request" : "500 Internal Server Error",
                                    "text/plain; charset=utf-8", error, strlen(error));
    }
    if (restoring) {
        close(fd);
        static const char message[] = "Backup validated and staged; rebooting to restore settings and sounds.\n";
        rc = server->sendResponse(cfd, "202 Accepted", "text/plain; charset=utf-8", message, sizeof(message) - 1);
        sync();
        sleep(1);
        if (reboot(RB_AUTOBOOT) < 0) {
            printf("[CONFIG] [ERR] restore staged, but reboot failed: %s; reboot manually\n", strerror(errno));
        }
        return rc;
    }
    off_t size = lseek(fd, 0, SEEK_END);
    if (size < 0) {
        int saved = errno;
        close(fd);
        return sendErrnoResponse(server, cfd, "500 Internal Server Error", "size backup", saved);
    }
    rc = server->sendResponse(cfd, "200 OK", "application/octet-stream", nullptr, (size_t)size, "audiox-backup.axb");
    char buf[65536];
    off_t pos = 0;
    while (rc == 0 && pos < size) {
        audioxWatchdogHeartbeat();
        size_t want = (uint64_t)(size - pos) > sizeof(buf) ? sizeof(buf) : (size_t)(size - pos);
        if (audioxUpdateReadAt(fd, buf, want, pos) < 0) {
            rc = -1;
            break;
        }
        size_t sent = 0;
        while (sent < want) {
            audioxWatchdogHeartbeat();
            ssize_t n = send(cfd, buf + sent, want - sent, MSG_NOSIGNAL);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { rc = -1; break; }
            sent += (size_t)n;
        }
        pos += (off_t)want;
    }
    if (rc < 0) printf("[CONFIG] [ERR] backup download interrupted: %s\n", strerror(errno));
    close(fd);
    return rc;
}

static ssize_t findHeaderEnd(const char *buf, size_t len) {
    if (!buf || len < 4) {
        return -1;
    }

    for (size_t i = 0; i + 3 < len; ++i) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') {
            return (ssize_t)(i + 4);
        }
    }
    for (size_t i = 0; i + 1 < len; ++i) {
        if (buf[i] == '\n' && buf[i + 1] == '\n') {
            return (ssize_t)(i + 2);
        }
    }
    return -1;
}

static ssize_t parseContentLength(const char *buf, size_t header_len) {
    if (!buf || header_len == 0) {
        return -1;
    }

    const char *line = buf;
    const char *end = buf + header_len;
    while (line < end) {
        const char *next = (const char *)memchr(line, '\n', (size_t)(end - line));
        size_t line_len = next ? (size_t)(next - line) : (size_t)(end - line);

        if (line_len >= 15 && strncasecmp(line, "Content-Length:", 15) == 0) {
            const char *p = line + 15;
            while (p < line + line_len && (*p == ' ' || *p == '\t')) {
                ++p;
            }
            long v = strtol(p, NULL, 10);
            if (v < 0) {
                return -1;
            }
            return (ssize_t)v;
        }

        if (!next) {
            break;
        }
        line = next + 1;
    }

    return -1;
}

static int setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

static void loadHtmlBody(HttpServer *server, const char *html_path) {
    if (!server) {
        return;
    }

    static const char fallback[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>audiox</title></head>"
        "<body><h1>audiox</h1><p>Web UI placeholder</p></body></html>\n";

    memset(server->html_path, 0, sizeof(server->html_path));
    const char *use_path = (html_path && html_path[0]) ? html_path : HTTP_DEFAULT_HTML_PATH;
    size_t p_len = strnlen(use_path, sizeof(server->html_path) - 1);
    memcpy(server->html_path, use_path, p_len);
    server->html_path[p_len] = '\0';

    size_t fb_len = sizeof(fallback) - 1;
    memcpy(server->body, fallback, fb_len);
    server->body_len = fb_len;

    int fd = open(server->html_path, O_RDONLY);
    if (fd < 0) {
        return;
    }

    ssize_t n = read(fd, server->body, sizeof(server->body));
    close(fd);
    if (n > 0) {
        server->body_len = (size_t)n;
    }
}

static void handleClient(HttpServer *server, int cfd) {
    char req[HTTP_REQ_MAX];
    size_t have = 0;
    ssize_t header_end = -1;

    while (have + 1 < sizeof(req)) {
        ssize_t n = recv(cfd, req + have, sizeof(req) - have - 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }

        have += (size_t)n;
        req[have] = '\0';

        header_end = findHeaderEnd(req, have);
        if (header_end >= 0) {
            break;
        }
    }

    if (header_end < 0) {
        static const char bad[] = "bad request\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }

    char method[8];
    char path[2048];
    method[0] = '\0';
    path[0] = '\0';
    int targetEnd = 0;
    if (sscanf(req, "%7s %2047s%n", method, path, &targetEnd) != 2 || req[targetEnd] != ' ') {
        static const char bad[] = "bad request line\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }

    char *query = strchr(path, '?');
    if (query) {
        *query++ = '\0';
    }

    ssize_t content_length = parseContentLength(req, (size_t)header_end);
    if (content_length < 0) {
        content_length = 0;
    }

    size_t body_off = (size_t)header_end;
    size_t initial_body_len = have > body_off ? have - body_off : 0;

    if (strcmp(path, "/api/config/backup") == 0 || strcmp(path, "/api/config/restore") == 0) {
        if (setSocketTimeouts(cfd, 10000, 10000) < 0) {
            (void)sendErrnoResponse(server, cfd, "500 Internal Server Error", "set backup transfer timeout", errno);
            return;
        }
        (void)handleConfigBackup(server, cfd, method, path, req + body_off, initial_body_len, (size_t)content_length);
        return;
    }

    if (strcmp(path, HTTP_BOOT_LOADER_PATH) == 0 ||
        strcmp(path, HTTP_BOOT_KERNEL_PATH) == 0) {
        if (strcmp(method, "PUT") == 0 &&
            setSocketTimeouts(cfd, 10000, 5000) < 0) {
            printf("[INIT] [WARN] failed to set boot upload socket timeout: %s\n", strerror(errno));
        }
        (void)handleBootFile(server,
                             cfd,
                             method,
                             path,
                             query,
                             (size_t)content_length,
                             req + body_off,
                             initial_body_len);
        return;
    }

    if (strcmp(path, HTTP_UPDATE_PATH) == 0) {
        if (strcmp(method, "PUT") != 0) {
            static const char mna[] = "method not allowed\n";
            (void)server->sendResponse(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
            return;
        }
        if ((size_t)content_length > HTTP_UPDATE_MAX) {
            static const char too_large[] = "payload too large\n";
            (void)server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
            return;
        }
        if (setSocketTimeouts(cfd, HTTP_INITRAM_RCV_TIMEOUT_SEC * 1000, 5000) < 0) {
            printf("[INIT] [WARN] failed to set update socket timeout: %s\n", strerror(errno));
        }
        (void)handleUpdateUpload(server,
                                 cfd,
                                 req + body_off,
                                 initial_body_len,
                                 (size_t)content_length);
        return;
    }

    if (strcmp(path, HTTP_INITRAM_PATH) == 0) {
        if (strcmp(method, "PUT") != 0) {
            static const char mna[] = "method not allowed\n";
            (void)server->sendResponse(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
            return;
        }
        if ((size_t)content_length > HTTP_INITRAM_MAX) {
            static const char too_large[] = "payload too large\n";
            (void)server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
            return;
        }
        if (setSocketTimeouts(cfd, HTTP_INITRAM_RCV_TIMEOUT_SEC * 1000, 5000) < 0) {
            printf("[INIT] [WARN] failed to extend upload socket timeout: %s\n", strerror(errno));
        }
        (void)handleInitramUpload(server,
                                  cfd,
                                  req + body_off,
                                  initial_body_len,
                                  (size_t)content_length);
        return;
    }

    if (strcmp(method, "PUT") == 0 &&
        strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX)) == 0) {
        if (setSocketTimeouts(cfd, 10000, 5000) < 0) {
            printf("[INIT] [WARN] failed to set rootfs upload socket timeout: %s\n", strerror(errno));
        }
        (void)handleRootfsUpload(server,
                                 cfd,
                                 path,
                                 req + body_off,
                                 initial_body_len,
                                 (size_t)content_length);
        return;
    }

    if ((size_t)content_length > HTTP_BODY_MAX) {
        static const char too_large[] = "payload too large\n";
        (void)server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
        return;
    }

    while ((have - body_off) < (size_t)content_length && have + 1 < sizeof(req)) {
        ssize_t n = recv(cfd, req + have, sizeof(req) - have - 1, 0);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            break;
        }
        have += (size_t)n;
        req[have] = '\0';
    }

    if ((have - body_off) < (size_t)content_length) {
        static const char bad[] = "incomplete body\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }

    const char *body = req + body_off;
    size_t body_len = (size_t)content_length;

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        if (server->sendFilePath(cfd, server->html_path, "text/html; charset=utf-8") < 0) {
            (void)server->sendResponse(cfd, "200 OK", "text/html; charset=utf-8", server->body, server->body_len);
        }
        return;
    }

    if (strcmp(method, "GET") == 0 && path[0] == '/' && strncmp(path, HTTP_API_PREFIX, strlen(HTTP_API_PREFIX)) != 0) {
        if (server->sendStaticFromWebRoot(cfd, path) == 0) {
            return;
        }
    }

    if (strncmp(path, HTTP_API_PREFIX, strlen(HTTP_API_PREFIX)) == 0) {
        if (handleApiRequest(server, cfd, method, path, body, body_len, query) == 0) {
            return;
        }
        static const char err[] = "api handler failed\n";
        (void)server->sendResponse(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return;
    }

    static const char not_found[] = "not found\n";
    (void)server->sendResponse(cfd, "404 Not Found", "text/plain; charset=utf-8", not_found, sizeof(not_found) - 1);
}

} // namespace

const char *audioxDetectBoard() {
    return detectBoard();
}

HttpServer::HttpServer(Audiox *context) : app(context) {
    listen_fd = -1;
    started = 0;
    html_path[0] = '\0';
    body_len = 0;

    if (app) {
        app->http = this;
    }

    loadHtmlBody(this, HTTP_DEFAULT_HTML_PATH);
}

int HttpServer::startSocket() {
    if (started) {
        return RET_OK;
    }

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        printf("[INIT] [ERR] HTTP socket failed: %s\n", strerror(errno));
        return RET_WARN;
    }

    int yes = 1;
    (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    if (setNonBlocking(listen_fd) < 0) {
        printf("[INIT] [ERR] HTTP fcntl non-blocking failed: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return RET_WARN;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(80);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[INIT] [WARN] HTTP bind failed on port 80: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return RET_WARN;
    }

    if (listen(listen_fd, 4) < 0) {
        printf("[INIT] [WARN] HTTP listen failed: %s\n", strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return RET_WARN;
    }

    started = 1;
    printf("[INIT] HTTP server listening on port 80\n");
    return RET_OK;
}

void HttpServer::poll() {
    if (!started || listen_fd < 0) {
        return;
    }

    while (1) {
        int cfd = accept(listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return;
            }
            return;
        }

        // Allow enough time for browser PUT/POST bodies to arrive without
        // tripping a socket timeout that appears as NetworkError in fetch().
        (void)setSocketTimeouts(cfd, 1500, 1500);

        handleClient(this, cfd);
        close(cfd);
    }
}

int HttpServer::sendResponse(int cfd,
                             const char *status,
                             const char *content_type,
                             const void *body_data,
                             size_t body_size,
                             const char *download_name) const {
    if (cfd < 0 || !status || !content_type) {
        return -1;
    }

    char disposition[128] = {};
    if (download_name) {
        int n = snprintf(disposition, sizeof(disposition),
                         "Content-Disposition: attachment; filename=\"%s\"\r\nCache-Control: no-store\r\n", download_name);
        if (n <= 0 || (size_t)n >= sizeof(disposition)) return -1;
    }
    char hdr[512];
    int hlen = snprintf(hdr,
                        sizeof(hdr),
                        "HTTP/1.1 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "%s"
                        "Connection: close\r\n\r\n",
                        status,
                        content_type,
                        body_size,
                        disposition);
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) {
        return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)hlen) {
        ssize_t n = send(cfd, hdr + sent, (size_t)hlen - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        sent += (size_t)n;
    }

    const char *body_bytes = (const char *)body_data;
    size_t body_sent = 0;
    while (body_bytes && body_sent < body_size) {
        ssize_t n = send(cfd, body_bytes + body_sent, body_size - body_sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            return -1;
        }
        body_sent += (size_t)n;
    }

    return 0;
}

int HttpServer::sendFilePath(int cfd,
                             const char *path,
                             const char *content_type_override) const {
    if (!path || !path[0]) {
        return -1;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return -1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0 || !S_ISREG(st.st_mode) || st.st_size < 0) {
        close(fd);
        return -1;
    }

    const char *ctype = content_type_override;
    if (!ctype) {
        const char *dot = strrchr(path, '.');
        if (!dot) {
            ctype = "application/octet-stream";
        } else if (strcasecmp(dot, ".html") == 0 || strcasecmp(dot, ".htm") == 0) {
            ctype = "text/html; charset=utf-8";
        } else if (strcasecmp(dot, ".css") == 0) {
            ctype = "text/css; charset=utf-8";
        } else if (strcasecmp(dot, ".js") == 0) {
            ctype = "application/javascript; charset=utf-8";
        } else if (strcasecmp(dot, ".json") == 0) {
            ctype = "application/json; charset=utf-8";
        } else if (strcasecmp(dot, ".txt") == 0) {
            ctype = "text/plain; charset=utf-8";
        } else if (strcasecmp(dot, ".svg") == 0) {
            ctype = "image/svg+xml";
        } else if (strcasecmp(dot, ".png") == 0) {
            ctype = "image/png";
        } else if (strcasecmp(dot, ".jpg") == 0 || strcasecmp(dot, ".jpeg") == 0) {
            ctype = "image/jpeg";
        } else if (strcasecmp(dot, ".gif") == 0) {
            ctype = "image/gif";
        } else if (strcasecmp(dot, ".webp") == 0) {
            ctype = "image/webp";
        } else if (strcasecmp(dot, ".ico") == 0) {
            ctype = "image/x-icon";
        } else if (strcasecmp(dot, ".wav") == 0) {
            ctype = "audio/wav";
        } else {
            ctype = "application/octet-stream";
        }
    }

    if (sendResponse(cfd, "200 OK", ctype, NULL, (size_t)st.st_size) < 0) {
        close(fd);
        return -1;
    }

    char buf[8192];
    while (1) {
        ssize_t nr = read(fd, buf, sizeof(buf));
        if (nr < 0) {
            if (errno == EINTR) {
                continue;
            }
            close(fd);
            return -1;
        }
        if (nr == 0) {
            break;
        }

        size_t off = 0;
        while (off < (size_t)nr) {
            ssize_t nw = send(cfd, buf + off, (size_t)nr - off, 0);
            if (nw < 0) {
                if (errno == EINTR) {
                    continue;
                }
                close(fd);
                return -1;
            }
            if (nw == 0) {
                close(fd);
                return -1;
            }
            off += (size_t)nw;
        }
    }

    close(fd);
    return 0;
}

int HttpServer::sendStaticFromWebRoot(int cfd, const char *path) const {
    if (!path || !path[0] || path[0] != '/') {
        return -1;
    }
    if (strstr(path, "..") != NULL || strchr(path, '\\') != NULL) {
        return -1;
    }

    char req_path[256];
    size_t req_len = strnlen(path, sizeof(req_path) - 1);
    memcpy(req_path, path, req_len);
    req_path[req_len] = '\0';

    char *query = strchr(req_path, '?');
    if (query) {
        *query = '\0';
    }

    char web_root[512];
    size_t root_len = strnlen(html_path, sizeof(web_root) - 1);
    if (root_len == 0) {
        return -1;
    }
    memcpy(web_root, html_path, root_len);
    web_root[root_len] = '\0';

    char *slash = strrchr(web_root, '/');
    if (!slash) {
        return -1;
    }
    *slash = '\0';

    if (!web_root[0]) {
        return -1;
    }

    char full[768];
    int n = snprintf(full, sizeof(full), "%s%s", web_root, req_path);
    if (n <= 0 || (size_t)n >= sizeof(full)) {
        return -1;
    }

    return sendFilePath(cfd, full, NULL);
}
