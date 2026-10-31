#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <pthread.h>

#include "../context.hpp"
#include "bluetooth/hci.hpp"
#include "audio/bluetooth_stream.hpp"
#include <sbc/sbc.h>

#define BT_MAX_DEVICES 32
#define BT_MESSAGE_MAX 128
// Devices connected at once; each link owns audio slot <index> on both sides.
#define BT_MAX_LINKS AUDIO_BT_MAX_LINKS

struct BluetoothDevice {
    bt::Address address;
    char name[BT_NAME_MAX];
    bool classic;
    bool paired;
    bool autoConnect;
    // Known A2DP roles. A device with neither role known gets both routing nodes.
    // audioSource: sends audio to AudioX (phone). audioSink: plays AudioX output.
    bool audioSource;
    bool audioSink;
    // Consecutive refused A2DP output channels while roles are unknown (not saved).
    uint8_t outputRefusals;
    // Skipped by auto-connect after the user or the device ended the connection
    // (not saved). Cleared by Connect, Auto-connect, or the device connecting in.
    bool autoConnectHeld;
    // Per-device auto-connect retry time and backoff (not saved).
    uint64_t autoConnectAtMs;
    uint64_t autoConnectBackoffMs;
    uint8_t linkKey[16];
    uint8_t linkKeyType;
};

enum BluetoothRequest : uint8_t {
    BT_REQUEST_NONE = 0,
    BT_REQUEST_SCAN = 1,
    BT_REQUEST_PAIR = 2,
    BT_REQUEST_CONNECT = 3,
    BT_REQUEST_DISCONNECT = 4,
    BT_REQUEST_UNPAIR = 5,
};

// One connected (or connecting) device. A single AVDTP signaling channel
// carries both of its streams: the input (remote source -> local sink SEID 1)
// and the output (local source SEID 2 -> remote sink). Only the worker
// thread touches links.
struct BluetoothLink {
    bool used;
    bt::Address address;
    int signalingFd;
    // Outgoing L2CAP connect still in progress.
    bool signalingConnecting;
    // User-visible "connecting": until the output starts, or the signaling
    // channel is up for an input-only device.
    bool connecting;
    uint64_t deadlineMs;
    bool wantOutput;
    bool wantInput;
    bool discovered;
    uint64_t discoverAtMs;
    uint8_t remoteSinkSeid;
    uint8_t remoteSourceSeid;

    // Commands AudioX sends as initiator; one outstanding at a time.
    uint8_t transaction;
    uint8_t pendingSignal;
    uint8_t pendingTransaction;
    bool pendingInput;
    uint64_t pendingSinceMs;
    uint8_t commandPacket[64];
    size_t commandLength;
    bool commandWritePending;

    // Input stream.
    bool inConfigured;
    bool inOpened;
    bool inStreaming;
    // The device configured the input itself (AudioX is acceptor).
    bool inRemoteActive;
    bool inMediaConnecting;
    int inMediaFd;
    uint64_t inInitiateAtMs;
    sbc_t decoder;
    bool decoderReady;
    uint8_t decoderChannels;
    uint8_t inConfig[4];
    uint8_t fragment[2048];
    size_t fragmentSize;
    bool fragmentActive;
    uint64_t inStatsNextMs;
    uint32_t inUnderrunsLogged;
    uint32_t inOverflowsLogged;
    uint32_t inDroppedLogged;
    // Arrival diagnostics for the current stats interval.
    uint64_t inStatsStartMs;
    uint64_t inLastPacketMs;
    uint32_t inPackets;
    uint32_t inFrames;
    uint32_t inMaxGapMs;
    uint32_t inLongGaps;
    uint32_t inLostPackets;
    uint32_t inSkippedFrames;
    uint32_t inDecodeErrors;
    bool inRtpValid;
    uint16_t inRtpSequence;
    uint32_t inRtpNextTimestamp;

    // Output stream.
    bool outStarting;
    bool outConfigured;
    bool outOpened;
    bool outUp;
    bool outStreaming;
    bool outMediaConnecting;
    bool outSuspendRequested;
    bool outSuspendSupported;
    bool outStartRequested;
    int outMediaFd;
    sbc_t encoder;
    bool encoderReady;
    uint16_t mediaMtu;
    uint8_t packet[1024];
    size_t packetLength;
    uint32_t rtpSequence;
    uint32_t rtpTimestamp;
    uint64_t silenceSinceMs;
    uint64_t statsNextMs;
    uint32_t droppedAtStats;
};

// Snapshot of a link for the web/framebuffer threads.
struct BluetoothLinkStatus {
    bool used;
    bt::Address address;
    bool connecting;
    bool outputConnected;
    bool outputStreaming;
    bool inputConnected;
    bool inputStreaming;
};

// A paired device as exposed to the routing graph.
struct BluetoothRoutingDevice {
    bt::Address address;
    char name[BT_NAME_MAX];
    bool input;
    bool output;
    bool connected;
};

// Owns the controller. All HCI traffic happens on the worker thread; the
// public request/status calls only touch state under the mutex.
struct BluetoothContext {
    Audiox *app;
    pthread_t thread;
    bool threadStarted;
    std::atomic<int> run;
    mutable std::mutex mutex;
    std::mutex pairedFileMutex;

    int devId;
    int agentFd;
    int sdpListenFd;
    int avdtpListenFd;
    int sdpClientFds[4];
    bool adapterReady;
    bool scanning;
    bool pairing;
    uint8_t request;
    bt::Address pairTarget;
    // A connect request is queued for the worker (guards duplicate requests).
    bool connectQueued;
    char message[BT_MESSAGE_MAX];
    BluetoothDevice devices[BT_MAX_DEVICES];
    size_t deviceCount;
    BluetoothLink links[BT_MAX_LINKS];
    // Published by the worker under mutex.
    BluetoothLinkStatus linkStatus[BT_MAX_LINKS];
    bool realtimeScheduling;
    size_t autoConnectNextDevice;
    uint64_t lastAutoConnectMs;
    uint64_t autoConnectCheckMs;
    uint64_t adapterRetryMs;

    BluetoothContext(Audiox *context);
    ~BluetoothContext();

    int start();
    void stop();

    // RET_WARN when busy.
    int requestScan();
    // RET_ERR for a bad address, RET_WARN when busy.
    int requestPair(const char *address);
    int requestConnect(const char *address);
    // nullptr or an empty address disconnects every Bluetooth audio connection.
    int requestDisconnect(const char *address = nullptr);
    int requestUnpair(const char *address);
    int requestAutoConnect(const char *address, bool enabled);
    int buildStatusJson(char *out, size_t outSize);
    // Name of the connected Bluetooth output, else of a paired output device.
    bool getOutputDeviceName(char *out, size_t outSize, bool *connected = nullptr) const;
    size_t copyRoutingDevices(BluetoothRoutingDevice *out, size_t cap) const;

    void threadMain();

private:
    bool bringUpAdapter();
    bool openA2dpListeners();
    void closeA2dpSockets();
    void acceptSdpRequest();
    void acceptA2dpChannel();
    bool handleSdpRequest(int fd);
    int findLink(const bt::Address &address) const;
    int allocateLink(const bt::Address &address);
    bool beginConnect(const bt::Address &target, bool automatic);
    void handleSignalingConnected(int index);
    void handleSignalingReadable(int index);
    void handleAvdtpCommand(int index, const uint8_t *packet, size_t length);
    void handleResponse(int index, uint8_t messageType, uint8_t signal, const uint8_t *payload, size_t length);
    void handleDiscoverResponse(int index, const uint8_t *payload, size_t length);
    void handleOutputResponse(int index, uint8_t messageType, uint8_t signal, const uint8_t *payload, size_t length);
    void handleInputResponse(int index, uint8_t messageType, uint8_t signal, const uint8_t *payload, size_t length);
    bool sendCommand(int index, uint8_t signal, const uint8_t *payload, size_t length, bool input);
    void flushCommand(int index);
    void driveLink(int index);
    void serviceLinkTimers(int index, uint64_t nowMs);
    bool connectMedia(int index, bool input);
    void handleMediaConnected(int index, bool input);
    bool readMediaMtu(int index);
    void readInputMedia(int index);
    void handleA2dpMedia(int index, const uint8_t *packet, size_t length);
    bool configureSbcDecoder(int index, const uint8_t configuration[4]);
    uint32_t decodeSbcPayload(int index, const uint8_t *payload, size_t length);
    void startInputAudio(int index);
    void logInputStats(int index, uint64_t nowMs, bool force);
    void pumpOutputAudio(int index);
    void flushOutputPacket(int index);
    void closeInputStream(int index, const char *reason);
    void closeOutputStream(int index, const char *reason);
    void closeLink(int index, const char *reason, bool hold = false);
    void closeAllLinks(const char *reason);
    bool linkIdle(int index) const;
    void publishLinkStatus();
    void runAutoConnect(uint64_t nowMs);
    void scheduleAutoConnectRetry(const bt::Address &address);
    void resetAutoConnectBackoff(const bt::Address &address);
    void holdAutoConnect(const bt::Address &address);
    bool pairingAllowed(const bt::Address &address);
    void runScan();
    void runPair(const bt::Address &target);
    void runUnpair(const bt::Address &target);
    void handleAgentEvent(const uint8_t *packet, size_t length);
    void setDeviceRoles(const bt::Address &address, int source, int sink, const char *reason);
    void noteOutputChannelResult(const bt::Address &address, bool refused);
    void setMessage(const char *format, ...) __attribute__((format(printf, 2, 3)));
    BluetoothDevice *findDevice(const bt::Address &address);
    const BluetoothDevice *findDevice(const bt::Address &address) const;
    void deviceLabel(const bt::Address &address, char *out, size_t outSize);
    void loadPaired();
    bool savePaired();
    // Last inbound ACL whose Class of Device reports a phone; applied when it pairs.
    bt::Address inboundPhone;

    // Pairing progress, only touched by the worker thread.
    bool pairKeyStored;
    int pairFailure;
};
