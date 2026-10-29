#pragma once

#include "http/context.hpp"

int handleSystemLogs(HttpServer *server, int cfd, const char *method, const char *query);
