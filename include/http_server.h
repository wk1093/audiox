#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <strings.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define HTTP_REQ_MAX 65536
#define HTTP_BODY_MAX 49152
#define HTTP_API_PREFIX "/api/"
#define HTTP_ROOTFS_PREFIX HTTP_API_PREFIX "rootfs/"
#define HTTP_SOUNDBOARD_TRIGGER_PREFIX HTTP_API_PREFIX "soundboard/trigger/"
#define HTTP_CONFIG_RELOAD_PATH HTTP_API_PREFIX "config/reload"
#define HTTP_FS_ROOT "/audiox/"

typedef int (*http_soundboard_trigger_fn)(void *ctx, int slot);
typedef int (*http_config_reload_fn)(void *ctx);

typedef struct http_server {
    int listen_fd;
    pthread_t thread;
    int started;
    char body[16384];
    size_t body_len;
    http_soundboard_trigger_fn soundboard_trigger;
    void *soundboard_ctx;
    http_config_reload_fn config_reload;
    void *config_reload_ctx;
} http_server_t;

static inline void http_server_load_body(http_server_t *srv, const char *html_path) {
    if (!srv) {
        return;
    }

    static const char fallback[] =
        "<!doctype html><html><head><meta charset=\"utf-8\"><title>audiox</title></head>"
        "<body><h1>audiox</h1><p>Web UI placeholder</p></body></html>\n";

    srv->body_len = 0;
    if (!html_path || !html_path[0]) {
        size_t n = sizeof(fallback) - 1;
        memcpy(srv->body, fallback, n);
        srv->body_len = n;
        return;
    }

    int fd = open(html_path, O_RDONLY);
    if (fd < 0) {
        size_t n = sizeof(fallback) - 1;
        memcpy(srv->body, fallback, n);
        srv->body_len = n;
        printf("[INIT] [WARN] HTTP content file missing (%s), using fallback page.\n", html_path);
        return;
    }

    ssize_t nread = read(fd, srv->body, sizeof(srv->body));
    close(fd);

    if (nread <= 0) {
        size_t n = sizeof(fallback) - 1;
        memcpy(srv->body, fallback, n);
        srv->body_len = n;
        printf("[INIT] [WARN] HTTP content file empty (%s), using fallback page.\n", html_path);
        return;
    }

    srv->body_len = (size_t)nread;
}

static inline int http_send_all(int fd, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = send(fd, p + sent, len - sent, 0);
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

    return 0;
}

static inline int http_send_response(int fd,
                                     const char *status,
                                     const char *content_type,
                                     const void *body,
                                     size_t body_len) {
    char hdr[256];
    int hlen = snprintf(hdr,
                        sizeof(hdr),
                        "HTTP/1.1 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        status,
                        content_type,
                        body_len);
    if (hlen <= 0) {
        return -1;
    }
    if (http_send_all(fd, hdr, (size_t)hlen) < 0) {
        return -1;
    }
    if (body_len > 0 && body) {
        if (http_send_all(fd, body, body_len) < 0) {
            return -1;
        }
    }
    return 0;
}

static inline ssize_t http_find_header_end(const char *buf, size_t len) {
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

static inline ssize_t http_parse_content_length(const char *buf, size_t header_len) {
    if (!buf || header_len == 0) {
        return -1;
    }

    const char *line = buf;
    const char *end = buf + header_len;
    while (line < end) {
        const char *next = memchr(line, '\n', (size_t)(end - line));
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

static inline int http_safe_api_suffix(const char *suffix) {
    if (!suffix || !suffix[0]) {
        return 0;
    }
    if (strstr(suffix, "..") != NULL) {
        return 0;
    }
    if (strchr(suffix, '\\') != NULL) {
        return 0;
    }
    return 1;
}

static inline int http_path_is_rootfs(const char *path) {
    if (!path) {
        return 0;
    }
    if (strcmp(path, "/api/rootfs") == 0) {
        return 1;
    }
    if (strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX)) == 0) {
        return 1;
    }
    return 0;
}

static inline const char *http_rootfs_suffix(const char *path) {
    if (!path) {
        return NULL;
    }
    if (strcmp(path, "/api/rootfs") == 0) {
        return "";
    }
    if (strncmp(path, HTTP_ROOTFS_PREFIX, strlen(HTTP_ROOTFS_PREFIX)) == 0) {
        return path + strlen(HTTP_ROOTFS_PREFIX);
    }
    return NULL;
}

static inline int http_is_unreserved_url_char(char c) {
    if ((c >= 'a' && c <= 'z') ||
        (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') ||
        c == '-' || c == '_' || c == '.' || c == '~') {
        return 1;
    }
    return 0;
}

static inline size_t http_url_encode_component(const char *src, char *dst, size_t dst_sz) {
    static const char hex[] = "0123456789ABCDEF";
    size_t out = 0;

    if (!dst || dst_sz == 0) {
        return 0;
    }

    if (!src) {
        dst[0] = '\0';
        return 0;
    }

    for (size_t i = 0; src[i] != '\0'; ++i) {
        unsigned char ch = (unsigned char)src[i];
        if (http_is_unreserved_url_char((char)ch)) {
            if (out + 1 >= dst_sz) {
                break;
            }
            dst[out++] = (char)ch;
            continue;
        }

        if (out + 3 >= dst_sz) {
            break;
        }
        dst[out++] = '%';
        dst[out++] = hex[(ch >> 4) & 0x0F];
        dst[out++] = hex[ch & 0x0F];
    }

    dst[out] = '\0';
    return out;
}

static inline size_t http_html_escape_append(char *dst, size_t dst_sz, size_t off, const char *src) {
    if (!dst || dst_sz == 0 || !src) {
        return off;
    }

    for (size_t i = 0; src[i] != '\0'; ++i) {
        const char *rep = NULL;
        size_t rep_len = 0;
        char ch = src[i];

        if (ch == '&') {
            rep = "&amp;";
            rep_len = 5;
        } else if (ch == '<') {
            rep = "&lt;";
            rep_len = 4;
        } else if (ch == '>') {
            rep = "&gt;";
            rep_len = 4;
        } else if (ch == '"') {
            rep = "&quot;";
            rep_len = 6;
        }

        if (rep) {
            if (off + rep_len + 1 >= dst_sz) {
                return off;
            }
            memcpy(dst + off, rep, rep_len);
            off += rep_len;
        } else {
            if (off + 2 >= dst_sz) {
                return off;
            }
            dst[off++] = ch;
        }
    }

    dst[off] = '\0';
    return off;
}

static inline int http_send_rootfs_dir_listing(int cfd,
                                               const char *api_path,
                                               const char *fs_path) {
    DIR *dir = opendir(fs_path);
    if (!dir) {
        static const char err[] = "opendir failed\n";
        (void)http_send_response(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return 0;
    }

    char base_href[300];
    size_t base_len = 0;
    if (api_path && api_path[0]) {
        base_len = strlen(api_path);
        if (base_len > sizeof(base_href) - 2) {
            base_len = sizeof(base_href) - 2;
        }
        memcpy(base_href, api_path, base_len);
    }
    if (base_len == 0) {
        base_len = strnlen(HTTP_ROOTFS_PREFIX, sizeof(base_href) - 1);
        memcpy(base_href, HTTP_ROOTFS_PREFIX, base_len);
    }
    if (base_len > 0 && base_href[base_len - 1] != '/') {
        base_href[base_len++] = '/';
    }
    base_href[base_len] = '\0';

    char body[HTTP_BODY_MAX];
    size_t off = 0;
    int n = snprintf(body,
                     sizeof(body),
                     "<!doctype html><html><head><meta charset=\"utf-8\">"
                     "<title>audiox rootfs</title></head><body><h1>audiox rootfs</h1><p>");
    if (n > 0) {
        off = (size_t)n;
    }
    off = http_html_escape_append(body, sizeof(body), off, api_path ? api_path : HTTP_ROOTFS_PREFIX);
    if (off + 16 < sizeof(body)) {
        memcpy(body + off, "</p><ul>", 7);
        off += 7;
        body[off] = '\0';
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }

        char full[512];
        int fn = snprintf(full, sizeof(full), "%s/%s", fs_path, ent->d_name);
        if (fn <= 0 || (size_t)fn >= sizeof(full)) {
            continue;
        }

        struct stat st;
        int is_dir = (stat(full, &st) == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;

        char enc[512];
        (void)http_url_encode_component(ent->d_name, enc, sizeof(enc));

        int wrote = snprintf(body + off,
                             sizeof(body) - off,
                             "<li><a href=\"%s%s%s\">",
                             base_href,
                             enc,
                             is_dir ? "/" : "");
        if (wrote <= 0 || (size_t)wrote >= sizeof(body) - off) {
            break;
        }
        off += (size_t)wrote;

        off = http_html_escape_append(body, sizeof(body), off, ent->d_name);
        if (is_dir) {
            if (off + 2 >= sizeof(body)) {
                break;
            }
            body[off++] = '/';
        }

        if (off + 11 >= sizeof(body)) {
            break;
        }
        memcpy(body + off, "</a></li>", 9);
        off += 9;
        body[off] = '\0';
    }

    closedir(dir);

    if (off + 20 >= sizeof(body)) {
        off = sizeof(body) - 32;
        memcpy(body + off, "<li>... truncated ...</li>", 24);
        off += 24;
    }
    memcpy(body + off, "</ul></body></html>\n", 20);
    off += 20;

    (void)http_send_response(cfd, "200 OK", "text/html; charset=utf-8", body, off);
    return 0;
}

static inline int http_ensure_parent_dirs(const char *path) {
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

static inline int http_handle_api_get(int cfd, const char *path) {
    const char *suffix = http_rootfs_suffix(path);
    if (!suffix) {
        static const char bad[] = "bad path\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    if (!http_safe_api_suffix(suffix)) {
        static const char bad[] = "bad path\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    char fs_path[512];
    int pn = snprintf(fs_path, sizeof(fs_path), "%s%s", HTTP_FS_ROOT, suffix);
    if (pn <= 0 || (size_t)pn >= sizeof(fs_path)) {
        static const char bad[] = "path too long\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    struct stat st;
    if (stat(fs_path, &st) < 0) {
        static const char nf[] = "not found\n";
        (void)http_send_response(cfd, "404 Not Found", "text/plain; charset=utf-8", nf, sizeof(nf) - 1);
        return 0;
    }

    if (S_ISDIR(st.st_mode)) {
        return http_send_rootfs_dir_listing(cfd, path, fs_path);
    }

    int fd = open(fs_path, O_RDONLY);
    if (fd < 0) {
        static const char err[] = "open failed\n";
        (void)http_send_response(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return 0;
    }

    char buf[HTTP_BODY_MAX];
    ssize_t n = read(fd, buf, sizeof(buf));
    close(fd);

    if (n < 0) {
        static const char err[] = "read failed\n";
        (void)http_send_response(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return 0;
    }

    (void)http_send_response(cfd, "200 OK", "application/octet-stream", buf, (size_t)n);
    return 0;
}

static inline int http_handle_api_put(int cfd, const char *path, const char *body, size_t body_len) {
    const char *suffix = http_rootfs_suffix(path);
    if (!suffix) {
        static const char bad[] = "bad path\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    if (!http_safe_api_suffix(suffix)) {
        static const char bad[] = "bad path\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    char fs_path[512];
    int pn = snprintf(fs_path, sizeof(fs_path), "%s%s", HTTP_FS_ROOT, suffix);
    if (pn <= 0 || (size_t)pn >= sizeof(fs_path)) {
        static const char bad[] = "path too long\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    if (http_ensure_parent_dirs(fs_path) < 0) {
        static const char err[] = "mkdir failed\n";
        (void)http_send_response(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return 0;
    }

    int fd = open(fs_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        static const char err[] = "open failed\n";
        (void)http_send_response(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return 0;
    }

    ssize_t nw = write(fd, body, body_len);
    close(fd);
    if (nw < 0 || (size_t)nw != body_len) {
        static const char err[] = "write failed\n";
        (void)http_send_response(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return 0;
    }

    static const char ok[] = "ok\n";
    (void)http_send_response(cfd, "200 OK", "text/plain; charset=utf-8", ok, sizeof(ok) - 1);
    return 0;
}

static inline int http_handle_soundboard_trigger(http_server_t *srv,
                                                 int cfd,
                                                 const char *path) {
    if (!srv || !path) {
        static const char bad[] = "bad request\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    if (!srv->soundboard_trigger) {
        static const char err[] = "soundboard trigger not configured\n";
        (void)http_send_response(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return 0;
    }

    const char *slot_str = path + strlen(HTTP_SOUNDBOARD_TRIGGER_PREFIX);
    if (!slot_str[0]) {
        static const char bad[] = "missing slot\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    char *endp = NULL;
    long slot_long = strtol(slot_str, &endp, 10);
    if (!endp || *endp != '\0' || slot_long < 0 || slot_long > 1024) {
        static const char bad[] = "invalid slot\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    int rc = srv->soundboard_trigger(srv->soundboard_ctx, (int)slot_long);
    if (rc == 0) {
        char out[48];
        int n = snprintf(out, sizeof(out), "triggered slot %ld\n", slot_long);
        if (n < 0) {
            n = 0;
        }
        (void)http_send_response(cfd, "200 OK", "text/plain; charset=utf-8", out, (size_t)n);
        return 0;
    }

    static const char err[] = "trigger failed\n";
    (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    return 0;
}

static inline int http_handle_config_reload(http_server_t *srv,
                                            int cfd) {
    if (!srv) {
        static const char bad[] = "bad request\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return 0;
    }

    if (!srv->config_reload) {
        static const char err[] = "config reload not configured\n";
        (void)http_send_response(cfd, "503 Service Unavailable", "text/plain; charset=utf-8", err, sizeof(err) - 1);
        return 0;
    }

    int rc = srv->config_reload(srv->config_reload_ctx);
    if (rc == 0) {
        static const char out[] = "config reloaded\n";
        (void)http_send_response(cfd, "200 OK", "text/plain; charset=utf-8", out, sizeof(out) - 1);
        return 0;
    }

    static const char err[] = "config reload failed\n";
    (void)http_send_response(cfd, "500 Internal Server Error", "text/plain; charset=utf-8", err, sizeof(err) - 1);
    return 0;
}

static inline void http_server_handle_client(http_server_t *srv, int cfd) {
    char req[HTTP_REQ_MAX];
    size_t have = 0;
    ssize_t header_end = -1;

    while (have + 1 < sizeof(req)) {
        ssize_t n = recv(cfd, req + have, sizeof(req) - have - 1, 0);
        if (n <= 0) {
            return;
        }
        have += (size_t)n;
        req[have] = '\0';

        header_end = http_find_header_end(req, have);
        if (header_end >= 0) {
            break;
        }
    }

    if (header_end < 0) {
        static const char bad[] = "bad request\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }

    char method[8];
    char path[256];
    method[0] = '\0';
    path[0] = '\0';
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        static const char bad[] = "bad request line\n";
        (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }

    if (strcmp(method, "GET") == 0 && (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0)) {
        (void)http_send_response(cfd, "200 OK", "text/html; charset=utf-8", srv->body, srv->body_len);
        return;
    }

    if (strncmp(path, HTTP_SOUNDBOARD_TRIGGER_PREFIX, strlen(HTTP_SOUNDBOARD_TRIGGER_PREFIX)) == 0) {
        if (strcmp(method, "GET") == 0 || strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
            (void)http_handle_soundboard_trigger(srv, cfd, path);
            return;
        }

        static const char mna[] = "method not allowed\n";
        (void)http_send_response(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
        return;
    }

    if (strcmp(path, HTTP_CONFIG_RELOAD_PATH) == 0) {
        if (strcmp(method, "POST") == 0 || strcmp(method, "PUT") == 0) {
            (void)http_handle_config_reload(srv, cfd);
            return;
        }

        static const char mna[] = "method not allowed\n";
        (void)http_send_response(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
        return;
    }

    if (http_path_is_rootfs(path)) {
        if (strcmp(method, "GET") == 0) {
            (void)http_handle_api_get(cfd, path);
            return;
        }

        if (strcmp(method, "PUT") == 0) {
            ssize_t content_len = http_parse_content_length(req, (size_t)header_end);
            if (content_len < 0 || content_len > HTTP_BODY_MAX) {
                static const char bad[] = "invalid content-length\n";
                (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
                return;
            }

            size_t need = (size_t)header_end + (size_t)content_len;
            while (have < need && have + 1 < sizeof(req)) {
                ssize_t n = recv(cfd, req + have, sizeof(req) - have - 1, 0);
                if (n <= 0) {
                    static const char bad[] = "short body\n";
                    (void)http_send_response(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
                    return;
                }
                have += (size_t)n;
                req[have] = '\0';
            }

            if (have < need) {
                static const char big[] = "request too large\n";
                (void)http_send_response(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", big, sizeof(big) - 1);
                return;
            }

            (void)http_handle_api_put(cfd, path, req + header_end, (size_t)content_len);
            return;
        }

        static const char mna[] = "method not allowed\n";
        (void)http_send_response(cfd, "405 Method Not Allowed", "text/plain; charset=utf-8", mna, sizeof(mna) - 1);
        return;
    }

    static const char not_found[] = "404 Not Found\n";
    (void)http_send_response(cfd,
                             "404 Not Found",
                             "text/plain; charset=utf-8",
                             not_found,
                             sizeof(not_found) - 1);
}

static inline void *http_server_thread_main(void *arg) {
    http_server_t *srv = (http_server_t *)arg;

    while (srv && srv->started && srv->listen_fd >= 0) {
        int cfd = accept(srv->listen_fd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR) {
                continue;
            }
            continue;
        }

        http_server_handle_client(srv, cfd);
        close(cfd);
    }

    return NULL;
}

static inline int http_server_start(http_server_t *srv,
                                    uint16_t port,
                                    const char *html_path,
                                    http_soundboard_trigger_fn trigger_fn,
                                    void *trigger_ctx,
                                    http_config_reload_fn config_reload_fn,
                                    void *config_reload_ctx) {
    if (!srv) {
        return -1;
    }

    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = -1;
    srv->soundboard_trigger = trigger_fn;
    srv->soundboard_ctx = trigger_ctx;
    srv->config_reload = config_reload_fn;
    srv->config_reload_ctx = config_reload_ctx;

    http_server_load_body(srv, html_path);

    srv->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (srv->listen_fd < 0) {
        printf("[INIT] [ERR] HTTP socket failed: %s\n", strerror(errno));
        return -1;
    }

    int yes = 1;
    (void)setsockopt(srv->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(srv->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        printf("[INIT] [ERR] HTTP bind failed on port %u: %s\n", (unsigned)port, strerror(errno));
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    if (listen(srv->listen_fd, 4) < 0) {
        printf("[INIT] [ERR] HTTP listen failed: %s\n", strerror(errno));
        close(srv->listen_fd);
        srv->listen_fd = -1;
        return -1;
    }

    srv->started = 1;
    if (pthread_create(&srv->thread, NULL, http_server_thread_main, srv) != 0) {
        printf("[INIT] [ERR] HTTP thread create failed: %s\n", strerror(errno));
        close(srv->listen_fd);
        srv->listen_fd = -1;
        srv->started = 0;
        return -1;
    }

    (void)pthread_detach(srv->thread);
    printf("[INIT] HTTP server listening on port %u\n", (unsigned)port);
    return 0;
}

#endif
