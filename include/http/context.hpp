#pragma once

#include "defs.hpp"
#include "../context.hpp"

#include <pthread.h>
#include <stddef.h>

int handleApiRequest(HttpServer *server,
                     int cfd,
                     const char *method,
                     const char *path,
                     const char *body,
                     size_t body_len);

struct HttpServer {
    Audiox *app;

    int listen_fd;
    pthread_t thread;
    int started;
    char html_path[512];
    char body[16384];
    size_t body_len;

    HttpServer(Audiox *context);

    int startSocket() WARN_UNUSED;

    void poll();

    int sendResponse(int cfd,
                     const char *status,
                     const char *content_type,
                     const void *body,
                     size_t body_len) const;

    int sendFilePath(int cfd,
                     const char *path,
                     const char *content_type_override) const;

    int sendStaticFromWebRoot(int cfd, const char *path) const;
};