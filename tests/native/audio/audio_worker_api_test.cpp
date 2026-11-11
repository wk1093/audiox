#include "../../../src/http/api.cpp"
#include "audio/worker.hpp"
#include <cassert>
#include <memory>
#include <sys/socket.h>

Audiox::Audiox() : audio(nullptr), midi(nullptr), touch(nullptr), fb(nullptr),
    config(nullptr), http(nullptr), bluetooth(nullptr), ready(false) {}
AudioContext::AudioContext(Audiox *context) : app(context) {
    routingGraphPublished = {};
    routingGraphPublished.generation = 1;
    memset(sfxSlots, 0, sizeof(sfxSlots));
    sfxQueueRead.store(0);
    sfxQueueWrite.store(0);
    pendingSfxStopAll.store(0);
    pendingSfxHoldStops.store(0);
    sfxPlayingCount = 0;
    memset(sfxPlayingBasenames, 0, sizeof(sfxPlayingBasenames));
    effectStatesSeq.store(1);
}
AudioContext::~AudioContext() { stopAudioWorker(); }
audiox::plugin::PluginHost::~PluginHost() {}
size_t AudioContext::copyDeviceInfos(AudioDeviceInfo *, size_t) const { return 0; }
namespace audiox::effects {
const char *effectTypeToString(uint8_t) { return "gain"; }
}

static std::string request(HttpServer &server, const char *method, const char *body = nullptr) {
    int sockets[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sockets) == 0);
    assert(handleAudioWorker(&server, sockets[0], method, body, body ? strlen(body) : 0) == 0);
    assert(shutdown(sockets[0], SHUT_WR) == 0);
    std::string response;
    char buffer[2048];
    ssize_t count;
    while ((count = read(sockets[1], buffer, sizeof(buffer))) > 0) response.append(buffer, count);
    close(sockets[0]);
    close(sockets[1]);
    return response;
}

int main(int argc, char **argv) {
    assert(argc == 2);
    Audiox app;
    HttpServer http(&app);
    assert(request(http, "GET").find("503 Service Unavailable") != std::string::npos);
    auto audio = std::make_unique<AudioContext>(&app);
    app.audio = audio.get();
    assert(request(http, "GET").find("\"running\":false") != std::string::npos);
    assert(request(http, "PUT").find("405 Method Not Allowed") != std::string::npos);
    assert(request(http, "POST", "enable_plugins=2").find("400 Bad Request") != std::string::npos);
    assert(request(http, "POST", "enable_plugins=badbadbadbad").find("400 Bad Request") != std::string::npos);
    assert(request(http, "POST", "other=1").find("400 Bad Request") != std::string::npos);
    assert(request(http, "POST", "enable_plugins=1").find("503 Service Unavailable") != std::string::npos);
    audio->workerSupervisor = new AudioWorkerSupervisor(*audio, argv[1]);
    assert(audio->workerSupervisor->start());
    const auto response = request(http, "POST");
    assert(response.find("202 Accepted") != std::string::npos);
    assert(response.find("{\"ok\":true,\"restart_requested\":true}") != std::string::npos);
    assert(request(http, "POST", "enable_plugins=1").find("202 Accepted") != std::string::npos);
    audio->stopAudioWorker();
    assert(!audio->processingThreadStarted);
    puts("Audio worker HTTP readiness, asynchronous restart, retry and invalid-input contracts passed");
}
