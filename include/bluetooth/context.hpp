#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <pthread.h>

#include "../context.hpp"
#include "bluetooth/hci.hpp"
#include <sbc/sbc.h>

#define BT_MAX_DEVICES 32
#define BT_MESSAGE_MAX 128

struct BluetoothDevice {
    bt::Address address;
    char name[BT_NAME_MAX];
    bool classic;
    bool paired;
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

// Owns the controller. All HCI traffic happens on the worker thread; the
// public request/status calls only touch state under the mutex.
struct BluetoothContext {
    Audiox *app;
    pthread_t thread;
    bool threadStarted;
    std::atomic<int> run;
    mutable std::mutex mutex;

    int devId;
    int agentFd;
    int sdpListenFd;
    int avdtpListenFd;
    int signalingFd;
    int mediaFd;
    int outputSignalingFd;
    int outputMediaFd;
    bool adapterReady;
    bool scanning;
    bool pairing;
    uint8_t request;
    bt::Address pairTarget;
    char message[BT_MESSAGE_MAX];
    BluetoothDevice devices[BT_MAX_DEVICES];
    size_t deviceCount;
    sbc_t sbcDecoder;
    bool sbcDecoderInitialized;
    uint8_t sbcChannels;
    bool avdtpConfigured;
    bool avdtpOpened;
    bool avdtpStreaming;
    uint8_t sbcConfiguration[4];
    uint8_t sbcFragment[2048];
    size_t sbcFragmentSize;
    bool sbcFragmentActive;
    sbc_t sbcEncoder;
    bool sbcEncoderInitialized;
    uint8_t outputSeid;
    uint8_t outputTransaction;
    uint8_t outputPendingSignal;
    uint32_t rtpSequence;
    uint32_t rtpTimestamp;
    bool outputConnecting;
    std::atomic<int> outputConnectingStatus;
    bool outputMediaConnecting;
    bool outputConfigured;
    bool outputOpened;
    std::atomic<int> outputConnected;
    uint8_t outputPacket[1024];
    size_t outputPacketLength;
    uint64_t outputStatsNextMs;
    uint32_t outputPacketsSinceStats;
    uint32_t outputDroppedAtStats;
    uint8_t outputFramesPerPacket;
    uint16_t outputMediaMtu;
    uint32_t outputSendWouldBlock;
    uint32_t outputWouldBlockAtStats;
    bool outputRealtimeScheduling;

    BluetoothContext(Audiox *context);
    ~BluetoothContext();

    int start();
    void stop();

    // RET_WARN when busy.
    int requestScan();
    // RET_ERR for a bad address, RET_WARN when busy.
    int requestPair(const char *address);
    int requestConnect(const char *address);
    int requestDisconnect();
    int requestUnpair(const char *address);
    int buildStatusJson(char *out, size_t outSize);
    bool getOutputDeviceName(char *out, size_t outSize) const;

    void threadMain();

private:
    bool bringUpAdapter();
    bool openA2dpListeners();
    void closeA2dpSockets();
    void acceptSdpRequest();
    void acceptA2dpChannel();
    void handleSdpRequest(int fd);
    void handleAvdtpPacket(const uint8_t *packet, size_t length);
    void handleA2dpMedia(const uint8_t *packet, size_t length);
    void resetA2dpStream();
    bool beginOutputConnect(const bt::Address &target);
    void handleOutputSignaling();
    void handleOutputMediaWritable();
    void sendOutputCommand(uint8_t signal, const uint8_t *payload, size_t length);
    void advanceOutputNegotiation(uint8_t signal, const uint8_t *payload, size_t length);
    bool connectOutputMedia();
    bool readOutputMediaMtu();
    void pumpOutputAudio();
    void disconnectOutput(const char *reason);
    void runScan();
    void runPair(const bt::Address &target);
    void runUnpair(const bt::Address &target);
    void handleAgentEvent(const uint8_t *packet, size_t length);
    void setMessage(const char *format, ...) __attribute__((format(printf, 2, 3)));
    BluetoothDevice *findDevice(const bt::Address &address);
    void loadPaired();
    void savePaired();
    bool configureSbcDecoder(const uint8_t configuration[4]);
    uint32_t decodeSbcPayload(const uint8_t *payload, size_t length);
    bt::Address outputTarget;

    // Pairing progress, only touched by the worker thread.
    bool pairKeyStored;
    int pairFailure;
};
