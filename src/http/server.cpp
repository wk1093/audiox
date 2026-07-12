#include "http/context.hpp"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_REQ_MAX 65536
#define HTTP_BODY_MAX 49152

#define HTTP_API_PREFIX "/api/"
#define HTTP_DEFAULT_HTML_PATH "/etc/www/index.html"

namespace {

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

    ssize_t content_length = parseContentLength(req, (size_t)header_end);
    if (content_length < 0) {
        content_length = 0;
    }
    if ((size_t)content_length > HTTP_BODY_MAX) {
        static const char too_large[] = "payload too large\n";
        (void)server->sendResponse(cfd, "413 Payload Too Large", "text/plain; charset=utf-8", too_large, sizeof(too_large) - 1);
        return;
    }

    size_t body_off = (size_t)header_end;
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

    char method[8];
    char path[256];
    method[0] = '\0';
    path[0] = '\0';
    if (sscanf(req, "%7s %255s", method, path) != 2) {
        static const char bad[] = "bad request line\n";
        (void)server->sendResponse(cfd, "400 Bad Request", "text/plain; charset=utf-8", bad, sizeof(bad) - 1);
        return;
    }

    char *query = strchr(path, '?');
    if (query) {
        *query = '\0';
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
        if (handleApiRequest(server, cfd, method, path, body, body_len) == 0) {
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

        struct timeval io_to;
        io_to.tv_sec = 0;
        io_to.tv_usec = 50000;
        (void)setsockopt(cfd, SOL_SOCKET, SO_RCVTIMEO, &io_to, sizeof(io_to));
        (void)setsockopt(cfd, SOL_SOCKET, SO_SNDTIMEO, &io_to, sizeof(io_to));

        handleClient(this, cfd);
        close(cfd);
    }
}

int HttpServer::sendResponse(int cfd,
                             const char *status,
                             const char *content_type,
                             const void *body_data,
                             size_t body_size) const {
    if (cfd < 0 || !status || !content_type) {
        return -1;
    }

    char hdr[256];
    int hlen = snprintf(hdr,
                        sizeof(hdr),
                        "HTTP/1.1 %s\r\n"
                        "Content-Type: %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        status,
                        content_type,
                        body_size);
    if (hlen <= 0 || (size_t)hlen >= sizeof(hdr)) {
        return -1;
    }

    size_t sent = 0;
    while (sent < (size_t)hlen) {
        ssize_t n = send(cfd, hdr + sent, (size_t)hlen - sent, 0);
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
        ssize_t n = send(cfd, body_bytes + body_sent, body_size - body_sent, 0);
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

