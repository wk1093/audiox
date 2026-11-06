#include "http/scenes.hpp"
#include "config/scenes.hpp"
#include "json.hpp"
#include <cstdlib>
#include <cstring>
#include <map>

namespace {
bool queryFields(const char *query, std::map<std::string, std::string> &fields) {
    if (!query) return true;
    if (strlen(query) > 1024) return false;
    std::string text(query);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('&', start);
        if (end == std::string::npos) end = text.size();
        const std::string field = text.substr(start, end - start);
        const size_t equal = field.find('=');
        if (equal == std::string::npos || !fields.emplace(field.substr(0, equal), field.substr(equal + 1)).second)
            return false;
        start = end + 1;
    }
    return true;
}
bool number(const std::string &text, int &value) {
    if (text.empty()) return false;
    char *end;
    long parsed = strtol(text.c_str(), &end, 10);
    if (*end || parsed < -1 || parsed > 127) return false;
    value = static_cast<int>(parsed);
    return true;
}
}

int handleSceneRequest(HttpServer *server, int cfd, const char *method,
                       const char *path, const char *query) {
    using namespace audiox::scenes;
    auto reply = [&](const char *status, const std::string &text) {
        return server->sendResponse(cfd, status, "application/json", text.data(), text.size());
    };
    auto errorReply = [&](const char *status, const std::string &error) {
        return reply(status, "{\"error\":\"" + audiox::json::escape(error.c_str()) + "\"}\n");
    };
    Store store;
    std::string error;
    if (!strcmp(path, "/api/scenes")) {
        if (strcmp(method, "GET")) return errorReply("405 Method Not Allowed", "use GET");
        std::vector<Scene> scenes;
        if (!store.list(scenes, error)) return errorReply("503 Service Unavailable", error);
        std::string out = "{\"scenes\":[";
        bool first = true;
        for (const auto &scene : scenes) {
            if (!first) out += ",";
            first = false;
            out += "{\"id\":\"" + scene.id + "\",\"name\":\"" + audiox::json::escape(scene.name.c_str()) +
                "\",\"channel\":" + std::to_string(scene.channel) + ",\"note\":" + std::to_string(scene.note) +
                ",\"effects\":" + std::to_string(scene.effects.size()) + ",\"routes\":" +
                std::to_string(scene.routes.size()) + ",\"volumes\":" + std::to_string(scene.volumes.size()) + "}";
        }
        std::string active;
        uint32_t sequence = 0;
        if (server->app && server->app->audio) {
            std::lock_guard<std::mutex> lock(server->app->audio->sceneMutex);
            active = server->app->audio->activeSceneId;
            sequence = server->app->audio->sceneRecallSeq.load(std::memory_order_acquire);
        }
        return reply("200 OK", out + "],\"active\":\"" + active + "\",\"sequence\":" + std::to_string(sequence) + "}\n");
    }
    if (strcmp(path, "/api/scenes/save") && strcmp(path, "/api/scenes/bind") &&
        strcmp(path, "/api/scenes/recall") && strcmp(path, "/api/scenes/delete"))
        return errorReply("404 Not Found", "unknown scene endpoint");
    if (strcmp(method, "POST") && !(strcmp(path, "/api/scenes/delete") == 0 && !strcmp(method, "DELETE")))
        return errorReply("405 Method Not Allowed", "use POST (or DELETE for delete)");
    std::map<std::string, std::string> fields;
    if (!queryFields(query, fields) || !validId(fields["id"]))
        return errorReply("400 Bad Request", "invalid scene query or id");
    const std::string id = fields["id"];
    if (!strcmp(path, "/api/scenes/delete")) {
        if (!store.remove(id, error)) return errorReply("400 Bad Request", error);
        return reply("200 OK", "{\"ok\":true}\n");
    }
    Scene scene;
    if (!strcmp(path, "/api/scenes/save")) {
        scene.id = id;
        if (!decode(fields["name_hex"], scene.name) || !number(fields["channel"], scene.channel) ||
            !number(fields["note"], scene.note)) return errorReply("400 Bad Request", "invalid name or MIDI binding");
        if (!capture(server->app, scene, error)) return errorReply("400 Bad Request", error);
        if (!store.save(scene, error)) return errorReply("400 Bad Request", error);
    } else {
        if (!store.read(id, scene, error)) return errorReply("404 Not Found", error);
        if (!strcmp(path, "/api/scenes/bind")) {
            if (!number(fields["channel"], scene.channel) || !number(fields["note"], scene.note))
                return errorReply("400 Bad Request", "invalid MIDI binding");
            if (!store.save(scene, error)) return errorReply("400 Bad Request", error);
        } else if (!recall(server->app, scene, error)) return errorReply("503 Service Unavailable", error);
    }
    return reply("200 OK", "{\"ok\":true}\n");
}
