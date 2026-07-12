#include "http/context.hpp"


HttpServer::HttpServer(Audiox *context) : app(context) {
}

int HttpServer::startSocket() {
    return RET_WARN;
}

void HttpServer::poll() {
}

