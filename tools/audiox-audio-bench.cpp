#include "audio/engine.hpp"
#include "audio/pcm_convert.hpp"
#include "defs.hpp"

#include <alsa/asoundlib.h>
#include <sndfile.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

#ifndef AUDIOX_BENCH_HTML
#define AUDIOX_BENCH_HTML "web/audio-bench.html"
#endif

namespace {

constexpr uint32_t kBenchChannels = 2;
constexpr uint32_t kBlockFrames = BUFFER_FRAMES;
constexpr float kPi = 3.14159265358979323846f;
std::atomic<bool> running{true};

struct SourceAudio {
    std::vector<float> samples;
    uint32_t channels = 1;
    uint32_t sampleRate = SAMPLE_RATE;
    std::string name = "440 Hz test tone";
};

struct BenchState {
    audiox::AudioEngine engine{SAMPLE_RATE, kBenchChannels};
    SourceAudio source;
    std::atomic<bool> playing{true};
    std::atomic<int> audioReady{0};
    std::atomic<int> audioBits{0};
    std::atomic<uint64_t> renderedFrames{0};
    std::atomic<uint64_t> underruns{0};
    std::string audioDevice = "default";
    unsigned port = 8765;
};

void handleSignal(int) {
    running.store(false, std::memory_order_relaxed);
}

bool loadWave(const char* path, SourceAudio& source, std::string& error) {
    SF_INFO info = {};
    SNDFILE* file = sf_open(path, SFM_READ, &info);
    if (!file) {
        error = sf_strerror(nullptr);
        return false;
    }
    if (info.frames <= 0 || info.channels < 1 || info.channels > 2 || info.samplerate <= 0 ||
        static_cast<uint64_t>(info.frames) > static_cast<uint64_t>(info.samplerate) * 600U) {
        error = "WAV must be mono/stereo PCM and no longer than ten minutes";
        sf_close(file);
        return false;
    }

    source.samples.resize(static_cast<size_t>(info.frames) * static_cast<size_t>(info.channels));
    const sf_count_t readFrames = sf_readf_float(file, source.samples.data(), info.frames);
    sf_close(file);
    if (readFrames != info.frames) {
        error = "failed to read complete audio file";
        return false;
    }
    source.channels = static_cast<uint32_t>(info.channels);
    source.sampleRate = static_cast<uint32_t>(info.samplerate);
    source.name = path;
    return true;
}

bool openOutput(const std::string& device,
                snd_pcm_t** output,
                snd_pcm_format_t* outputFormat,
                std::string& error) {
    int rc = snd_pcm_open(output, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
    if (rc < 0) {
        error = snd_strerror(rc);
        return false;
    }
    const snd_pcm_format_t formats[] = {SND_PCM_FORMAT_S32_LE, SND_PCM_FORMAT_S16_LE};
    for (snd_pcm_format_t format : formats) {
        rc = snd_pcm_set_params(*output,
                                format,
                                SND_PCM_ACCESS_RW_INTERLEAVED,
                                kBenchChannels,
                                SAMPLE_RATE,
                                1,
                                50000);
        if (rc >= 0) {
            *outputFormat = format;
            return true;
        }
        (void)snd_pcm_close(*output);
        *output = nullptr;
        if (format == formats[0]) {
            rc = snd_pcm_open(output, device.c_str(), SND_PCM_STREAM_PLAYBACK, 0);
            if (rc < 0) break;
        }
    }
    if (rc < 0) {
        error = snd_strerror(rc);
        return false;
    }
    return false;
}

void audioLoop(BenchState* state) {
    std::vector<float> input(kBlockFrames * kBenchChannels);
    std::vector<float> output(kBlockFrames * kBenchChannels);
    std::vector<int16_t> pcm16(kBlockFrames * kBenchChannels);
    std::vector<int32_t> pcm32(kBlockFrames * kBenchChannels);
    double sourcePosition = 0.0;
    float oscillatorPhase = 0.0f;
    snd_pcm_t* pcmDevice = nullptr;
    snd_pcm_format_t pcmFormat = SND_PCM_FORMAT_UNKNOWN;
    std::string lastError;

    while (running.load(std::memory_order_relaxed)) {
        if (!pcmDevice) {
            std::string error;
            if (!openOutput(state->audioDevice, &pcmDevice, &pcmFormat, error)) {
                state->audioReady.store(-1, std::memory_order_relaxed);
                if (error != lastError) {
                    std::fprintf(stderr, "[bench] ALSA output unavailable: %s\n", error.c_str());
                    lastError = error;
                }
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            state->audioReady.store(1, std::memory_order_relaxed);
            state->audioBits.store(pcmFormat == SND_PCM_FORMAT_S32_LE ? 32 : 16,
                                   std::memory_order_relaxed);
            lastError.clear();
        }

        for (uint32_t frame = 0; frame < kBlockFrames; ++frame) {
            float left = 0.0f;
            float right = 0.0f;
            if (state->playing.load(std::memory_order_relaxed)) {
                if (state->source.samples.empty()) {
                    left = right = 0.2f * std::sin(oscillatorPhase);
                    oscillatorPhase += 2.0f * kPi * 440.0f / static_cast<float>(SAMPLE_RATE);
                    if (oscillatorPhase >= 2.0f * kPi) {
                        oscillatorPhase -= 2.0f * kPi;
                    }
                } else {
                    const uint64_t sourceFrame = static_cast<uint64_t>(sourcePosition) %
                        (state->source.samples.size() / state->source.channels);
                    const size_t base = static_cast<size_t>(sourceFrame) * state->source.channels;
                    left = state->source.samples[base];
                    right = state->source.channels == 1 ? left : state->source.samples[base + 1];
                    sourcePosition += static_cast<double>(state->source.sampleRate) / SAMPLE_RATE;
                }
            }
            input[static_cast<size_t>(frame) * kBenchChannels] = left;
            input[static_cast<size_t>(frame) * kBenchChannels + 1] = right;
        }

        if (state->engine.process(input.data(), output.data(), kBlockFrames) != 0) {
            state->underruns.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        for (size_t index = 0; index < output.size(); ++index) {
            if (pcmFormat == SND_PCM_FORMAT_S32_LE) {
                pcm32[index] = audiox::pcm::toS32(output[index]);
            } else {
                pcm16[index] = audiox::pcm::toS16(output[index]);
            }
        }

        const void* pcmData = pcmFormat == SND_PCM_FORMAT_S32_LE
            ? static_cast<const void*>(pcm32.data())
            : static_cast<const void*>(pcm16.data());
        snd_pcm_sframes_t written = snd_pcm_writei(pcmDevice, pcmData, kBlockFrames);
        if (written < 0) {
            written = snd_pcm_recover(pcmDevice, static_cast<int>(written), 1);
            if (written < 0) {
                state->audioReady.store(-1, std::memory_order_relaxed);
                snd_pcm_close(pcmDevice);
                pcmDevice = nullptr;
                state->underruns.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
        }
        state->renderedFrames.fetch_add(kBlockFrames, std::memory_order_relaxed);
        if (state->audioDevice == "null") {
            const auto blockDuration = std::chrono::nanoseconds(
                static_cast<int64_t>(1000000000ULL * kBlockFrames / SAMPLE_RATE));
            std::this_thread::sleep_for(blockDuration);
        }
    }

    if (pcmDevice) {
        snd_pcm_drain(pcmDevice);
        snd_pcm_close(pcmDevice);
    }
}

std::string urlDecode(const std::string& value) {
    std::string decoded;
    for (size_t index = 0; index < value.size(); ++index) {
        if (value[index] == '+') {
            decoded.push_back(' ');
        } else if (value[index] == '%' && index + 2 < value.size()) {
            char hex[3] = {value[index + 1], value[index + 2], '\0'};
            decoded.push_back(static_cast<char>(std::strtoul(hex, nullptr, 16)));
            index += 2;
        } else {
            decoded.push_back(value[index]);
        }
    }
    return decoded;
}

std::map<std::string, std::string> parseForm(const std::string& body) {
    std::map<std::string, std::string> fields;
    size_t start = 0;
    while (start <= body.size()) {
        const size_t end = body.find('&', start);
        const std::string field = body.substr(start, end == std::string::npos ? end : end - start);
        const size_t equals = field.find('=');
        if (equals != std::string::npos) {
            fields[urlDecode(field.substr(0, equals))] = urlDecode(field.substr(equals + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return fields;
}

std::string effectsJson(BenchState& state) {
    std::string json = "{\"effects\":[";
    const auto effects = state.engine.effects();
    for (size_t index = 0; index < effects.size(); ++index) {
        const auto& effect = effects[index];
        const auto* spec = audiox::effects::effectTypeSpecFor(effect.type);
        if (index) json += ',';
        json += "{\"id\":\"" + effect.id + "\",\"type\":\"" +
                audiox::effects::effectTypeToString(effect.type) + "\",\"enabled\":" +
                (effect.enabled ? "true" : "false") + ",\"params\":[";
        for (uint8_t param = 0; param < spec->paramCount; ++param) {
            if (param) json += ',';
            json += "{\"name\":\"" + std::string(spec->params[param].name) +
                    "\",\"label\":\"" + spec->params[param].label +
                    "\",\"min\":" + std::to_string(spec->params[param].minValue) +
                    ",\"max\":" + std::to_string(spec->params[param].maxValue) +
                    ",\"value\":" + std::to_string(effect.values[param]) + "}";
        }
        json += "]}";
    }
    json += "]}";
    return json;
}

std::string statusJson(const BenchState& state) {
    const double seconds = static_cast<double>(state.renderedFrames.load(std::memory_order_relaxed)) / SAMPLE_RATE;
    return "{\"playing\":" + std::string(state.playing.load() ? "true" : "false") +
           ",\"audioReady\":" + std::string(state.audioReady.load() == 1 ? "true" : "false") +
           ",\"audioDevice\":\"" + state.audioDevice + "\",\"source\":\"" + state.source.name +
           "\",\"sampleRate\":" + std::to_string(SAMPLE_RATE) +
           ",\"sampleFormatBits\":" + std::to_string(state.audioBits.load()) +
           ",\"renderedSeconds\":" + std::to_string(seconds) +
           ",\"underruns\":" + std::to_string(state.underruns.load()) + "}";
}

void sendResponse(int client, const char* status, const char* type, const std::string& body) {
    const std::string response = std::string("HTTP/1.1 ") + status + "\r\nContent-Type: " + type +
        "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n" + body;
    size_t sent = 0;
    while (sent < response.size()) {
        const ssize_t count = send(client, response.data() + sent, response.size() - sent, MSG_NOSIGNAL);
        if (count <= 0) return;
        sent += static_cast<size_t>(count);
    }
}

bool receiveRequest(int client, std::string& request) {
    constexpr size_t maximumHeaderBytes = 8192;
    constexpr size_t maximumBodyBytes = 4096;
    char buffer[2048];
    size_t headerEnd = std::string::npos;
    while (headerEnd == std::string::npos) {
        const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
        if (count <= 0) return false;
        request.append(buffer, static_cast<size_t>(count));
        if (request.size() > maximumHeaderBytes) return false;
        headerEnd = request.find("\r\n\r\n");
    }

    size_t contentLength = 0;
    std::istringstream headers(request.substr(0, headerEnd));
    std::string line;
    std::getline(headers, line);
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::transform(line.begin(), line.end(), line.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (line.rfind("content-length:", 0) == 0) {
            char* end = nullptr;
            const unsigned long parsed = std::strtoul(line.c_str() + 15, &end, 10);
            if (!end || *end != '\0' || parsed > maximumBodyBytes) return false;
            contentLength = static_cast<size_t>(parsed);
        }
    }

    while (request.size() - headerEnd - 4 < contentLength) {
        const ssize_t count = recv(client, buffer, sizeof(buffer), 0);
        if (count <= 0) return false;
        request.append(buffer, static_cast<size_t>(count));
        if (request.size() - headerEnd - 4 > maximumBodyBytes) return false;
    }
    return true;
}

void handleClient(int client, BenchState& state) {
    timeval receiveTimeout = {3, 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receiveTimeout, sizeof(receiveTimeout));
    std::string request;
    if (!receiveRequest(client, request)) {
        sendResponse(client, "400 Bad Request", "text/plain", "invalid or oversized request\n");
        return;
    }
    const size_t lineEnd = request.find("\r\n");
    const std::string requestLine = request.substr(0, lineEnd);
    const size_t firstSpace = requestLine.find(' ');
    const size_t secondSpace = requestLine.find(' ', firstSpace == std::string::npos ? firstSpace : firstSpace + 1);
    if (firstSpace == std::string::npos || secondSpace == std::string::npos) {
        sendResponse(client, "400 Bad Request", "text/plain", "bad request\n");
        return;
    }
    const std::string method = requestLine.substr(0, firstSpace);
    const std::string path = requestLine.substr(firstSpace + 1, secondSpace - firstSpace - 1);
    const size_t headerEnd = request.find("\r\n\r\n");
    const std::string body = headerEnd == std::string::npos ? "" : request.substr(headerEnd + 4);

    if (method == "GET" && (path == "/" || path == "/index.html")) {
        std::ifstream file(AUDIOX_BENCH_HTML, std::ios::binary);
        if (!file) {
            sendResponse(client, "500 Internal Server Error", "text/plain", "bench UI file not found\n");
            return;
        }
        const std::string html((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        sendResponse(client, "200 OK", "text/html; charset=utf-8", html);
    } else if (method == "GET" && path == "/api/status") {
        sendResponse(client, "200 OK", "application/json", statusJson(state));
    } else if (method == "GET" && path == "/api/effects") {
        sendResponse(client, "200 OK", "application/json", effectsJson(state));
    } else if (method == "POST" && path == "/api/effect") {
        const auto fields = parseForm(body);
        if (fields.count("id") == 0) {
            sendResponse(client, "400 Bad Request", "application/json", "{\"error\":\"missing id\"}");
            return;
        }
        const std::string id = fields.at("id");
        bool ok = true;
        if (fields.count("type")) {
            const std::string& type = fields.at("type");
            const bool validType = type == "gain" || type == "distortion" || type == "pitch" ||
                type == "reverb" || type == "gate" || type == "cut";
            if (!validType) {
                sendResponse(client, "400 Bad Request", "application/json", "{\"error\":\"invalid effect type\"}");
                return;
            }
            ok = state.engine.setEffectType(id, audiox::effects::effectTypeFromString(type.c_str())) == 0;
        }
        if (ok && fields.count("enabled")) {
            ok = state.engine.setEffectEnabled(id, fields.at("enabled") == "1") == 0;
        }
        sendResponse(client, ok ? "200 OK" : "404 Not Found", "application/json",
                     ok ? "{\"ok\":true}" : "{\"error\":\"effect not found\"}");
    } else if (method == "POST" && path == "/api/parameter") {
        const auto fields = parseForm(body);
        if (!fields.count("id") || !fields.count("name") || !fields.count("value")) {
            sendResponse(client, "400 Bad Request", "application/json", "{\"error\":\"missing parameter fields\"}");
            return;
        }
        char* end = nullptr;
        const float value = std::strtof(fields.at("value").c_str(), &end);
        const bool valid = end && *end == '\0' && std::isfinite(value);
        const int result = valid ? state.engine.setEffectParameter(fields.at("id"), fields.at("name").c_str(), value) : -1;
        sendResponse(client, result == 0 ? "200 OK" : "400 Bad Request", "application/json",
                     result == 0 ? "{\"ok\":true}" : "{\"error\":\"invalid effect parameter\"}");
    } else if (method == "POST" && path == "/api/transport") {
        const auto fields = parseForm(body);
        const auto playing = fields.find("playing");
        state.playing.store(playing != fields.end() && playing->second == "1", std::memory_order_relaxed);
        sendResponse(client, "200 OK", "application/json", "{\"ok\":true}");
    } else {
        sendResponse(client, "404 Not Found", "text/plain", "not found\n");
    }
}

int runHttpServer(BenchState& state) {
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        std::perror("socket");
        return 1;
    }
    int reuse = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = htons(static_cast<uint16_t>(state.port));
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(server, 8) < 0) {
        std::perror("bind/listen");
        close(server);
        return 1;
    }

    std::printf("[bench] UI: http://127.0.0.1:%u\n", state.port);
    while (running.load(std::memory_order_relaxed)) {
        pollfd event = {server, POLLIN, 0};
        const int ready = poll(&event, 1, 500);
        if (ready <= 0) {
            if (ready < 0 && errno != EINTR) std::perror("poll");
            continue;
        }
        const int client = accept(server, nullptr, nullptr);
        if (client < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            std::perror("accept");
            continue;
        }
        handleClient(client, state);
        close(client);
    }
    close(server);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    BenchState state;
    if (argc > 1 && std::strcmp(argv[1], "-") != 0) {
        std::string error;
        if (!loadWave(argv[1], state.source, error)) {
            std::fprintf(stderr, "Cannot load %s: %s\n", argv[1], error.c_str());
            return 2;
        }
    }
    if (argc > 2) state.audioDevice = argv[2];
    if (argc > 3) state.port = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
    if (state.port == 0 || state.port > 65535) {
        std::fprintf(stderr, "HTTP port must be between 1 and 65535\n");
        return 2;
    }

    for (uint8_t type = audiox::effects::EFFECT_GAIN; type <= audiox::effects::EFFECT_GATE; ++type) {
        const std::string id = "bench_fx_" + std::to_string(type);
        if (state.engine.addEffect(id, type, false) != 0) {
            std::fprintf(stderr, "Failed to initialize effect chain\n");
            return 1;
        }
    }

    std::signal(SIGINT, handleSignal);
    std::signal(SIGTERM, handleSignal);
    std::thread audioThread(audioLoop, &state);
    const int result = runHttpServer(state);
    running.store(false, std::memory_order_relaxed);
    audioThread.join();
    return result;
}
