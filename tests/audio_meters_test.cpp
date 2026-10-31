#include "../src/audio/graph.cpp"

#include <cassert>
#include <limits>
#include <memory>

AudioContext::AudioContext(Audiox *context) : app(context) {}
AudioContext::~AudioContext() {}

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
        "{\"id\":\"fx_\\u0022\\u005c\\u000a\",\"levels\":[0.0000]}]}\n") == 0);

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
    puts("Audio meter snapshot tests passed");
}
