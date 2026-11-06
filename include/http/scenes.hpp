#pragma once
#include "http/context.hpp"

int handleSceneRequest(HttpServer *server, int cfd, const char *method,
                       const char *path, const char *query);
