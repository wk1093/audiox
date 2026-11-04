#pragma once

#include "http/context.hpp"

int handleSystemLogs(HttpServer *server, int cfd, const char *method, const char *query);
int readSystemLogText(const char *source, unsigned limit, char *out, size_t capacity, size_t *length = nullptr);
