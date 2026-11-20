#pragma once

#include "defs.hpp"
#include "../context.hpp"

#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

constexpr size_t HTTP_WEBSOCKET_CLIENTS = 4;
constexpr size_t HTTP_WEBSOCKET_FRAME_MAX = 49152;

struct HttpWebSocketClient {
    int fd{-1};
    uint32_t topics{};
    uint64_t lastActivityMs{};
    uint64_t pingAtMs{};
    uint64_t topicAtMs[5]{};
    uint8_t input[4096]{};
    size_t inputSize{};
    uint8_t output[HTTP_WEBSOCKET_FRAME_MAX + 128]{};
    size_t outputSize{};
    size_t outputOffset{};
    bool closeAfterFlush{};
};

struct HttpServer;

bool buildHttpEventJson(HttpServer *server, const char *topic,
                        char *out, size_t capacity, size_t *length);

const char *audioxDetectBoard();

int handleApiRequest(HttpServer *server,
                     int cfd,
                     const char *method,
                     const char *path,
                     const char *body,
                     size_t body_len,
                     const char *query = nullptr);

struct HttpServer {
    Audiox *app;

    int listen_fd;
    pthread_t thread;
    int started;
    char html_path[512];
    char body[16384];
    size_t body_len;
    char requestHost[256]{};
    bool apiRequest{};
    HttpWebSocketClient websocketClients[HTTP_WEBSOCKET_CLIENTS]{};
    uint64_t websocketTopicAtMs[5]{};

    HttpServer(Audiox *context);

    int startSocket() WARN_UNUSED;

    void poll();

    int sendResponse(int cfd,
                     const char *status,
                     const char *content_type,
                     const void *body,
                     size_t body_len,
                     const char *download_name = nullptr) const;
    int sendResponseWithHeaders(int cfd, const char *status, const char *content_type,
                                const void *body, size_t body_len,
                                const char *download_name, const char *headers) const;

    int sendFilePath(int cfd,
                     const char *path,
                     const char *content_type_override) const;

    int sendStaticFromWebRoot(int cfd, const char *path) const;
};