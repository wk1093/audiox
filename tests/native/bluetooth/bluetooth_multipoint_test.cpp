#include "bluetooth/context.hpp"
#include "bluetooth/route_id.hpp"
#include "audio/context.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <memory>
#include <sys/socket.h>
#include <unistd.h>

namespace {
int mediaFd = -1;
int mediaError = 0;
unsigned mediaPackets = 0;

struct Fixture {
    Audiox app;
    std::unique_ptr<AudioContext> audio{std::make_unique<AudioContext>(&app)};
    BluetoothContext context{&app};
    int remoteSignal = -1;
    int remoteMedia = -1;

    Fixture() {
        app.audio = audio.get();
        context.deviceCount = 1;
        auto &device = context.devices[0];
        device.address.b[0] = 1;
        device.audioSink = true;
        snprintf(device.name, sizeof(device.name), "Multipoint headphones");
        assert(context.allocateLink(device.address) == 0);
        auto &link = context.links[0];
        int fds[2];
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
        link.signalingFd = fds[0];
        remoteSignal = fds[1];
        assert(socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK, 0, fds) == 0);
        link.outMediaFd = mediaFd = fds[0];
        remoteMedia = fds[1];
        link.wantOutput = link.discovered = true;
        link.outConfigured = link.outOpened = link.outUp = link.outStreaming = true;
        link.remoteSinkSeid = 3;
        const uint8_t config[] = {0x11, 0x15, 2, 20};
        assert(sbc_init_a2dp(&link.encoder, 0, config, sizeof(config)) == 0);
        link.encoderReady = true;
        auto &slot = audio->bluetoothOutputs[0];
        slot.peer.store(bt::packAddress(link.address.b));
        slot.signalActive.store(1);
        slot.setActive(true);
        link.statsNextMs = bt::monotonicMs() + 1000;
        mediaError = 0;
        mediaPackets = 0;
    }

    ~Fixture() {
        context.closeAllLinks(nullptr);
        close(remoteSignal);
        close(remoteMedia);
        mediaFd = -1;
    }

    void queueAudio() {
        int16_t pcm[4096];
        for (auto &sample : pcm) sample = 1000;
        assert(audio->bluetoothOutputs[0].push(pcm, 2048) == 2048);
    }
};

void checkBackpressure() {
    Fixture f;
    auto &link = f.context.links[0];
    auto &slot = f.audio->bluetoothOutputs[0];
    f.queueAudio();
    link.packetLength = 13;
    mediaError = EAGAIN;
    f.context.flushOutputPacket(0);
    assert(link.outBlockedSinceMs != 0);
    f.context.pumpOutputAudio(0);
    assert(link.used && link.pendingSignal == 0 && slot.active.load());

    link.outBlockedSinceMs = bt::monotonicMs() - 200;
    slot.dropped.store(SAMPLE_RATE);
    link.statsNextMs = bt::monotonicMs();
    f.context.pumpOutputAudio(0);
    assert(link.used && link.outUp && link.pendingSignal == 9);
    assert(!slot.active.load() && slot.available() == 0 && link.packetLength == 0);
    assert(slot.peer.load() != 0);
    f.context.pumpOutputAudio(0);
    assert(link.pendingSignal == 9 && !slot.active.load());
    f.context.handleResponse(0, 2, 9, nullptr, 0);
    assert(!link.outStreaming && link.outRetryAtMs > bt::monotonicMs());
    const uint64_t retryAt = link.outRetryAtMs;
    f.context.pumpOutputAudio(0);
    assert(link.pendingSignal == 0 && !slot.active.load());
    assert(retryAt - bt::monotonicMs() <= 5000);

    link.outRetryAtMs = bt::monotonicMs() - 1;
    f.context.pumpOutputAudio(0);
    assert(slot.active.load());
    f.queueAudio();
    f.context.pumpOutputAudio(0);
    assert(link.pendingSignal == 7 && mediaPackets == 0);
    mediaError = 0;
    f.context.handleResponse(0, 2, 7, nullptr, 0);
    assert(link.outStreaming && link.outRetryAtMs == 0);
    assert(strstr(f.context.message, "Connected to Multipoint headphones"));
    f.context.pumpOutputAudio(0);
    assert(link.used && mediaPackets == 1);
}

void checkBusyStartAndHandoff() {
    Fixture f;
    auto &link = f.context.links[0];
    auto &slot = f.audio->bluetoothOutputs[0];
    link.outUp = false;
    link.connecting = true;
    link.deadlineMs = bt::monotonicMs() + 30000;
    const uint8_t rejection[] = {3 << 2, 0x31};
    f.context.handleResponse(0, 3, 7, rejection, sizeof(rejection));
    assert(link.used && link.outUp && !link.outStreaming && !link.connecting);
    assert(!slot.active.load() && link.deadlineMs == 0);
    for (int attempt = 0; attempt < 3; ++attempt) {
        link.outRetryAtMs = bt::monotonicMs() - 1;
        f.context.pumpOutputAudio(0);
        f.queueAudio();
        f.context.pumpOutputAudio(0);
        assert(link.pendingSignal == 7);
        f.context.handleResponse(0, 3, 7, rejection, sizeof(rejection));
        assert(link.used && !slot.active.load() && slot.available() == 0);
    }
    f.context.handleResponse(0, 2, 7, nullptr, 0);
    const uint8_t suspend[] = {0x10, 9, 2 << 2};
    f.context.handleAvdtpCommand(0, suspend, sizeof(suspend));
    assert(link.used && !link.outStreaming && !slot.active.load());
    f.context.pumpOutputAudio(0);
    assert(link.pendingSignal == 0 && !slot.active.load());
    const uint8_t start[] = {0x20, 7, 2 << 2};
    f.context.handleAvdtpCommand(0, start, sizeof(start));
    assert(link.outStreaming && slot.active.load() && link.outRetryAtMs == 0);
}

void checkTransientBackpressure() {
    Fixture f;
    auto &link = f.context.links[0];
    link.packetLength = 13;
    mediaError = EINTR;
    f.context.flushOutputPacket(0);
    assert(link.used && link.packetLength == 13 && link.outBlockedSinceMs == 0);
    mediaError = EAGAIN;
    f.context.flushOutputPacket(0);
    assert(link.outBlockedSinceMs != 0);
    mediaError = 0;
    f.context.flushOutputPacket(0);
    assert(link.used && link.packetLength == 0 && link.outBlockedSinceMs == 0);
    assert(link.outRetryAtMs == 0 && mediaPackets == 1);
}

void checkHandoffDuringCommands() {
    Fixture f;
    auto &link = f.context.links[0];
    auto &slot = f.audio->bluetoothOutputs[0];
    link.pendingSignal = 7;
    link.pendingTransaction = 5;
    link.outStartRequested = true;
    const uint8_t suspend[] = {0x10, 9, 2 << 2};
    f.context.handleAvdtpCommand(0, suspend, sizeof(suspend));
    assert(link.pendingSignal == 0 && !link.outStreaming && !slot.active.load());
    const uint8_t staleStartResponse[] = {0x52, 7};
    assert(send(f.remoteSignal, staleStartResponse, sizeof(staleStartResponse), 0) == 2);
    f.context.handleSignalingReadable(0);
    assert(!link.outStreaming && link.outRetryAtMs != 0);

    link.pendingSignal = 9;
    link.pendingTransaction = 6;
    link.outSuspendRequested = true;
    const uint8_t start[] = {0x20, 7, 2 << 2};
    f.context.handleAvdtpCommand(0, start, sizeof(start));
    assert(link.pendingSignal == 0 && !link.outSuspendRequested);
    assert(link.outStreaming && slot.active.load());
    const uint8_t staleSuspendResponse[] = {0x62, 9};
    assert(send(f.remoteSignal, staleSuspendResponse, sizeof(staleSuspendResponse), 0) == 2);
    f.context.handleSignalingReadable(0);
    assert(link.outStreaming && slot.active.load());
}

void checkOverrunWithBackpressure() {
    Fixture f;
    auto &link = f.context.links[0];
    auto &slot = f.audio->bluetoothOutputs[0];
    link.packetLength = 13;
    mediaError = EAGAIN;
    f.context.flushOutputPacket(0);
    link.statsNextMs = bt::monotonicMs();
    slot.dropped.store(SAMPLE_RATE);
    f.context.pumpOutputAudio(0);
    assert(link.used && link.outRetryAtMs != 0 && !slot.active.load());
    f.context.pumpOutputAudio(0);
    assert(link.pendingSignal == 9);
}

void checkIdlePauseResume() {
    Fixture f;
    auto &link = f.context.links[0];
    auto &slot = f.audio->bluetoothOutputs[0];
    slot.signalActive.store(0);
    link.silenceSinceMs = bt::monotonicMs() - 2500;
    f.context.pumpOutputAudio(0);
    assert(link.pendingSignal == 9 && link.outRetryAtMs == 0);
    f.context.handleResponse(0, 2, 9, nullptr, 0);
    assert(!link.outStreaming && !slot.active.load());
    f.context.pumpOutputAudio(0);
    assert(link.pendingSignal == 0 && !slot.active.load());
    slot.signalActive.store(1);
    f.context.pumpOutputAudio(0);
    assert(slot.active.load());
    f.queueAudio();
    f.context.pumpOutputAudio(0);
    assert(link.pendingSignal == 7);
    slot.dropped.store(SAMPLE_RATE);
    link.statsNextMs = bt::monotonicMs();
    f.context.pumpOutputAudio(0);
    assert(link.used && link.pendingSignal == 7);
    f.context.handleResponse(0, 2, 7, nullptr, 0);
    f.context.pumpOutputAudio(0);
    assert(link.used && link.outStreaming && mediaPackets == 1);
}

void checkBusyDiscovery() {
    Fixture f;
    auto &link = f.context.links[0];
    link.outConfigured = link.outUp = link.outStarting = false;
    link.connecting = true;
    const uint8_t busy[] = {(3 << 2) | 2, 8};
    f.context.handleDiscoverResponse(0, busy, sizeof(busy));
    assert(!link.discovered && link.deadlineMs == 0);
    assert(link.discoverAtMs > bt::monotonicMs());
    f.context.serviceLinkTimers(0, bt::monotonicMs());
    assert(link.used && link.pendingSignal == 0);
    link.discoverAtMs = 0;
    f.context.driveLink(0);
    assert(link.pendingSignal == 1);
    const uint8_t available[] = {3 << 2, 8};
    f.context.handleResponse(0, 2, 1, available, sizeof(available));
    assert(link.used && link.pendingSignal == 2 && link.deadlineMs > bt::monotonicMs());

    const uint8_t inUse[] = {0, 0x13};
    f.context.handleResponse(0, 3, 3, inUse, sizeof(inUse));
    assert(link.used && !link.discovered && !link.outStarting && link.deadlineMs == 0);
}

void checkSafetyFallbacks() {
    for (const int error : {EPIPE, ECONNRESET}) {
        Fixture f;
        f.context.links[0].packetLength = 13;
        mediaError = error;
        f.context.flushOutputPacket(0);
        assert(!f.context.links[0].used);
    }
    {
        Fixture f;
        f.context.links[0].statsNextMs = bt::monotonicMs();
        f.audio->bluetoothOutputs[0].dropped.store(SAMPLE_RATE);
        f.context.pumpOutputAudio(0);
        assert(!f.context.links[0].used);
    }
    for (const bool timeout : {false, true}) {
        Fixture f;
        auto &link = f.context.links[0];
        f.context.waitForOutput(0, "test backpressure");
        f.context.pumpOutputAudio(0);
        assert(link.pendingSignal == 9);
        if (timeout) {
            f.context.serviceLinkTimers(0, link.pendingSinceMs + 10000);
        } else {
            const uint8_t reject[] = {3 << 2, 0x31};
            f.context.handleResponse(0, 3, 9, reject, sizeof(reject));
        }
        assert(!link.used);
    }
    {
        Fixture f;
        const uint8_t reject[] = {3 << 2, 0x12};
        f.context.handleResponse(0, 3, 7, reject, sizeof(reject));
        assert(!f.context.links[0].used);
    }
    {
        Fixture f;
        auto &link = f.context.links[0];
        link.outSuspendSupported = false;
        f.context.waitForOutput(0, "test backpressure");
        f.context.pumpOutputAudio(0);
        assert(!link.used);
    }
    {
        Fixture f;
        const uint8_t reject[] = {3 << 2, 0x31};
        f.context.handleResponse(0, 3, 9, reject, sizeof(reject));
        assert(f.context.links[0].used && !f.context.links[0].outSuspendSupported);
    }
}
}

extern "C" ssize_t __real_send(int, const void *, size_t, int);
extern "C" ssize_t __wrap_send(int fd, const void *data, size_t length, int flags) {
    if (fd == mediaFd) {
        if (mediaError) {
            errno = mediaError;
            return -1;
        }
        ++mediaPackets;
    }
    return __real_send(fd, data, length, flags);
}

Audiox::Audiox() : audio(nullptr), midi(nullptr), touch(nullptr), fb(nullptr),
    config(nullptr), http(nullptr), bluetooth(nullptr), ready(false) {}
AudioContext::AudioContext(Audiox *context) : app(context) {}
AudioContext::~AudioContext() {}
int AudioContext::reloadRoutingGraph() { return RET_OK; }

int main() {
    checkBackpressure();
    checkTransientBackpressure();
    checkHandoffDuringCommands();
    checkOverrunWithBackpressure();
    checkIdlePauseResume();
    checkBusyStartAndHandoff();
    checkBusyDiscovery();
    checkSafetyFallbacks();
    puts("Bluetooth multipoint waiting, resume, and safety fallback tests passed");
}
