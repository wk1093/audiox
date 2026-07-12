#include "config/context.hpp"
#include "audio/context.hpp"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>

namespace {

static constexpr size_t ROUTE_TEXT_MAX = 256;
static constexpr size_t ROUTE_COUNT_MAX = AUDIO_GRAPH_MAX_EDGES;

static void trimRouteText(char *text) {
    if (!text) {
        return;
    }
    while (*text == ' ' || *text == '\t') {
        memmove(text, text + 1, strlen(text));
    }
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t' || text[len - 1] == '\n' || text[len - 1] == '\r')) {
        text[len - 1] = '\0';
        --len;
    }
}

static int loadRoutes(std::vector<std::string> *routes) {
    if (!routes) {
        return RET_ERR;
    }

    routes->clear();
    FILE *fp = fopen(ROUTING_REAL_FILE_PATH, "r");
    if (!fp) {
        if (errno == ENOENT) {
            return RET_OK;
        }
        printf("[CONFIG] [WARN] failed to open %s for reading: %s\n", ROUTING_REAL_FILE_PATH, strerror(errno));
        return RET_ERR;
    }

    char line[ROUTE_TEXT_MAX];
    while (fgets(line, sizeof(line), fp)) {
        trimRouteText(line);
        if (!line[0] || line[0] == '#') {
            continue;
        }
        if (strncmp(line, "edge=", 5) != 0) {
            continue;
        }
        if (routes->size() >= ROUTE_COUNT_MAX) {
            break;
        }
        routes->emplace_back(line + 5);
    }

    if (ferror(fp)) {
        printf("[CONFIG] [WARN] failed while reading %s: %s\n", ROUTING_REAL_FILE_PATH, strerror(errno));
        fclose(fp);
        return RET_ERR;
    }

    fclose(fp);
    return RET_OK;
}

static int saveRoutes(const std::vector<std::string> &routes) {
    FILE *fp = fopen(ROUTING_REAL_FILE_PATH, "w");
    if (!fp) {
        printf("[CONFIG] [WARN] failed to open %s for writing: %s\n", ROUTING_REAL_FILE_PATH, strerror(errno));
        return RET_ERR;
    }

    fprintf(fp, "# audiox routing v1\n");
    for (size_t i = 0; i < routes.size() && i < ROUTE_COUNT_MAX; ++i) {
        fprintf(fp, "edge=%s\n", routes[i].c_str());
    }
    fprintf(fp, "\n");

    if (fclose(fp) != 0) {
        printf("[CONFIG] [WARN] failed to finalize %s: %s\n", ROUTING_REAL_FILE_PATH, strerror(errno));
        return RET_ERR;
    }
    return RET_OK;
}

} // namespace

RouterConfig::RouterConfig() {}

int RouterConfig::getRouteCount() const {
    std::vector<std::string> routes;
    int rc = loadRoutes(&routes);
    if (rc != RET_OK) {
        return -1;
    }
    return (int)routes.size();
}

void RouterConfig::getRoute(int index, char *out, size_t out_sz) const {
    if (!out || out_sz == 0) {
        return;
    }
    out[0] = '\0';

    std::vector<std::string> routes;
    if (loadRoutes(&routes) != RET_OK) {
        return;
    }
    if (index < 0 || (size_t)index >= routes.size()) {
        return;
    }

    size_t n = strnlen(routes[(size_t)index].c_str(), out_sz - 1);
    memcpy(out, routes[(size_t)index].c_str(), n);
    out[n] = '\0';
}

void RouterConfig::setRoute(int index, const char *route) {
    if (!route) {
        return;
    }

    std::vector<std::string> routes;
    if (loadRoutes(&routes) != RET_OK) {
        return;
    }
    if (index < 0 || (size_t)index >= routes.size()) {
        return;
    }

    routes[(size_t)index] = route;
    (void)saveRoutes(routes);
}

void RouterConfig::addRoute(const char *route) {
    if (!route || !route[0]) {
        return;
    }

    std::vector<std::string> routes;
    if (loadRoutes(&routes) != RET_OK) {
        return;
    }
    if (routes.size() >= ROUTE_COUNT_MAX) {
        return;
    }

    routes.emplace_back(route);
    (void)saveRoutes(routes);
}

void RouterConfig::removeRoute(int index) {
    std::vector<std::string> routes;
    if (loadRoutes(&routes) != RET_OK) {
        return;
    }
    if (index < 0 || (size_t)index >= routes.size()) {
        return;
    }

    routes.erase(routes.begin() + index);
    (void)saveRoutes(routes);
}