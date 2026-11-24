#include "../../../src/audio/graph.cpp"

#include <cassert>
#include <limits>
#include <memory>
#include <vector>

AudioContext::AudioContext(Audiox *context) : app(context) {}
AudioContext::~AudioContext() {}

static bool startsWith(const char *text, const char *prefix) {
    return !strncmp(text, prefix, strlen(prefix));
}

int main() {
    auto audio = std::make_unique<AudioContext>(nullptr);
    char json[49152];
    assert(audio->buildMetersJson(json, sizeof(json)) == RET_OK);
    assert(strcmp(json, "{\"meters\":[]}\n") == 0);
    assert(audio->buildMetersJson(nullptr, sizeof(json)) == RET_ERR);
    assert(audio->buildMetersJson(json, 0) == RET_ERR);
    assert(audio->buildMetersJson(json, 4) == RET_ERR);

    auto &graph = audio->routingGraphPublished;
    graph.thingCount = 3;
    strcpy(graph.things[0].id, "bt_A4C6F0A57AA7_in");
    graph.things[0].outputs = 2;
    strcpy(graph.things[1].id, "bt_B8841102C99C_out");
    graph.things[1].inputs = 2;
    strcpy(graph.things[2].id, "fx_\"\\\n");
    graph.things[2].inputs = 2;
    graph.things[2].outputs = 1;
    audio->nodeChannelLevels[0][0].store(0.5f);
    audio->nodeChannelLevels[0][1].store(0.25f);
    audio->nodeChannelLevels[1][0].store(2.0f);
    audio->nodeChannelLevels[1][1].store(-1.0f);
    audio->nodeChannelLevels[2][0].store(std::numeric_limits<float>::quiet_NaN());
    assert(audio->buildMetersJson(json, sizeof(json)) == RET_OK);
    assert(strcmp(json,
        "{\"meters\":[{\"id\":\"bt_A4C6F0A57AA7_in\",\"levels\":[0.5000,0.2500]},"
        "{\"id\":\"bt_B8841102C99C_out\",\"levels\":[1.0000,0.0000]},"
        "{\"id\":\"fx_\\\"\\\\\\u000a\",\"levels\":[0.0000]}]}\n") == 0);

    // The full supported graph and channel count must fit the HTTP response.
    graph.thingCount = AUDIO_GRAPH_MAX_THINGS;
    for (unsigned i = 0; i < AUDIO_GRAPH_MAX_THINGS; ++i) {
        memset(graph.things[i].id, '\n', sizeof(graph.things[i].id) - 1);
        graph.things[i].id[sizeof(graph.things[i].id) - 1] = '\0';
        graph.things[i].outputs = 16;
        for (unsigned ch = 0; ch < 16; ++ch) audio->nodeChannelLevels[i][ch].store(1.0f);
    }
    assert(audio->buildMetersJson(json, sizeof(json)) == RET_OK);
    assert(strlen(json) < sizeof(json));

    // A full-scale 1 kHz sine peaks near 0 dBFS in the band containing 1 kHz.
    std::vector<float> sine(audiox::spectrum::kFftSize);
    for (uint32_t i = 0; i < sine.size(); ++i)
        sine[i] = std::sin(2.0f * 3.14159265f * 1000.0f * i / SAMPLE_RATE);
    float bands[audiox::spectrum::kBands];
    audiox::spectrum::computeBands(sine.data(), SAMPLE_RATE, bands);
    uint32_t peak = 0;
    for (uint32_t band = 1; band < audiox::spectrum::kBands; ++band)
        if (bands[band] > bands[peak]) peak = band;
    assert(audiox::spectrum::bandEdgeHz(peak, SAMPLE_RATE) <= 1000.0f &&
           audiox::spectrum::bandEdgeHz(peak + 1, SAMPLE_RATE) >= 1000.0f);
    assert(bands[peak] > -1.6f && bands[peak] < 0.5f);
    assert(bands[0] < -60.0f && bands[audiox::spectrum::kBands - 1] < -60.0f);
    std::fill(sine.begin(), sine.end(), 0.0f);
    audiox::spectrum::computeBands(sine.data(), SAMPLE_RATE, bands);
    for (float db : bands) assert(db == audiox::spectrum::kFloorDb);

    // Requests select a graph index and report bands only once the worker matches it.
    graph.thingCount = 2;
    strcpy(graph.things[0].id, "fx_1");
    strcpy(graph.things[1].id, "fx_2");
    assert(audio->buildSpectrumJson("fx_9", json, sizeof(json)) == RET_WARN);
    assert(audio->spectrumNode.load() == -1);
    assert(audio->buildSpectrumJson("fx_2", json, sizeof(json)) == RET_OK);
    assert(audio->spectrumNode.load() == 1);
    assert(audio->spectrumUntilMs.load() > audiox::healthMonotonicMs());
    assert(startsWith(json, "{\"active\":false,\"min_hz\":20,\"max_hz\":20000,\"bands\":[-120.0,"));
    audio->spectrumBands[0].store(-6.04f);
    audio->spectrumBands[1].store(std::numeric_limits<float>::quiet_NaN());
    audio->spectrumBandsNode.store(1);
    assert(audio->buildSpectrumJson("fx_2", json, sizeof(json)) == RET_OK);
    assert(startsWith(json, "{\"active\":true,\"min_hz\":20,\"max_hz\":20000,\"bands\":[-6.0,-120.0,"));
    assert(audio->buildSpectrumJson("fx_2", json, 16) == RET_ERR);
    puts("Audio meter snapshot tests passed");
}
