#include "../../desktop/src/auto_routing.hpp"
#include <cassert>

int main() {
    desktop::AutoRouting routing(-1);
    desktop::Rule rule;
    rule.app = "policy test";
    rule.appRegex = true;
    rule.target = "test_quad";
    rule.pair = 1;
    desktop::Rule capture;
    capture.capture = true;
    capture.app = "AudioX capture policy test";
    capture.target = "test_mic";
    assert(routing.setRules({rule, capture}));
    bool ready = false;
    for (unsigned i = 0; i < 10 && !ready; ++i) {
        ready = routing.tick();
        if (!ready) usleep(300000);
    }
    assert(ready);
    std::string metadata;
    assert(desktop::command("pw-metadata", {"-n", "default", "-d", "0", "audiox.routing"}, -1, metadata));
    assert(routing.tick());
    assert(desktop::command("pw-metadata", {"-n", "default"}, -1, metadata));
    assert(metadata.find("key:'audiox.routing'") != std::string::npos);
    assert(metadata.find("policy test") != std::string::npos);
    assert(desktop::command("pw-metadata", {"-n", "default", "-d", "0", "audiox.policy"}, -1, metadata));
    for (unsigned i = 0; i < 3; ++i) assert(!routing.tick());
    assert(desktop::command("pw-metadata", {"-n", "default", "0", "audiox.policy", "1", "Spa:String"}, -1, metadata));
    assert(routing.tick());
    std::vector<desktop::PulseDevice> sinks;
    assert(desktop::pulseDevices(-1, false, sinks));
    std::string destination;
    for (const auto &sink : sinks)
        if (sink.name.find("audiox_auto_p_") == 0 && sink.name.back() == '2') destination = sink.name;
    assert(!destination.empty());
    std::string links;
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const pid_t player = fork();
        assert(player >= 0);
        if (!player) {
            execlp("pw-cat", "pw-cat", "--playback", "--raw", "-P",
                   "{ application.name = \"AudioX policy test\" }", "/dev/zero", nullptr);
            _exit(127);
        }
        usleep(300000);
        assert(desktop::command("pw-link", {"-l"}, -1, links));
        assert(links.find("pw-cat:output_FL\n  |-> " + destination + ":playback_FL") != std::string::npos);
        assert(links.find("pw-cat:output_FR\n  |-> " + destination + ":playback_FR") != std::string::npos);
        assert(links.find("pw-cat:output_FL\n  |-> decoy:playback_FL") == std::string::npos);
        assert(kill(player, SIGTERM) == 0);
        assert(waitpid(player, nullptr, 0) == player);
    }
    assert(desktop::command("pw-link", {"-l"}, -1, links));
    assert(links.find("output." + destination + ":output_RL\n  |-> test_quad:playback_RL") != std::string::npos);
    assert(links.find("output." + destination + ":output_RR\n  |-> test_quad:playback_RR") != std::string::npos);
    std::vector<desktop::PulseDevice> sources;
    assert(desktop::pulseDevices(-1, true, sources));
    std::string source;
    for (const auto &device : sources)
        if (device.name.find("audiox_auto_c_") == 0) source = device.name;
    assert(!source.empty());
    const pid_t recorder = fork();
    assert(recorder >= 0);
    if (!recorder) {
        execlp("pw-cat", "pw-cat", "--record", "--raw", "-P",
               "{ application.name = \"AudioX capture policy test\" }", "/dev/null", nullptr);
        _exit(127);
    }
    usleep(300000);
    assert(desktop::command("pw-link", {"-l"}, -1, links));
    assert(links.find("pw-cat:input_FL\n  |<- " + source + ":capture_FL") != std::string::npos);
    assert(links.find("input." + source + ":input_FL\n  |<- test_mic:capture_FL") != std::string::npos);
    assert(kill(recorder, SIGTERM) == 0);
    assert(waitpid(recorder, nullptr, 0) == recorder);
    assert(routing.setRules({}, false));
    assert(routing.tick());
    usleep(300000);
    assert(desktop::pulseDevices(-1, false, sinks));
    for (const auto &sink : sinks) assert(sink.name.find("audiox_auto_p_") != 0);
    assert(routing.setRules({rule, capture}, true));
    ready = false;
    for (unsigned i = 0; i < 10 && !ready; ++i) {
        ready = routing.tick();
        if (!ready) usleep(300000);
    }
    assert(ready);
    assert(desktop::pulseDevices(-1, false, sinks));
    bool recreated = false;
    for (const auto &sink : sinks) if (sink.name == destination) recreated = true;
    assert(recreated);
}
