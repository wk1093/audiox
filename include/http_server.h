#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct http_server {
    int listen_fd;
    pthread_t thread;
    int started;
    char body[16384];
    size_t body_len;
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

static inline void http_server_handle_client(http_server_t *srv, int cfd) {
    char req[1024];
    ssize_t n = recv(cfd, req, sizeof(req) - 1, 0);
    if (n <= 0) {
        return;
    }
    req[n] = '\0';

    int ok = 0;
    if (strncmp(req, "GET / ", 6) == 0 || strncmp(req, "GET /index.html", 15) == 0) {
        ok = 1;
    }

    char hdr[256];
    int hlen = 0;
    if (ok) {
        hlen = snprintf(hdr,
                        sizeof(hdr),
                        "HTTP/1.1 200 OK\r\n"
                        "Content-Type: text/html; charset=utf-8\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: close\r\n\r\n",
                        srv->body_len);
        if (hlen > 0) {
            (void)http_send_all(cfd, hdr, (size_t)hlen);
            (void)http_send_all(cfd, srv->body, srv->body_len);
        }
        return;
    }

    static const char not_found[] = "404 Not Found\n";
    hlen = snprintf(hdr,
                    sizeof(hdr),
                    "HTTP/1.1 404 Not Found\r\n"
                    "Content-Type: text/plain; charset=utf-8\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n\r\n",
                    sizeof(not_found) - 1);
    if (hlen > 0) {
        (void)http_send_all(cfd, hdr, (size_t)hlen);
        (void)http_send_all(cfd, not_found, sizeof(not_found) - 1);
    }
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

static inline int http_server_start(http_server_t *srv, uint16_t port, const char *html_path) {
    if (!srv) {
        return -1;
    }

    memset(srv, 0, sizeof(*srv));
    srv->listen_fd = -1;

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
