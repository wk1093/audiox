#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <pthread.h>

#include "../context.hpp"
#include "bluetooth/hci.hpp"

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
};

// Owns the controller. All HCI traffic happens on the worker thread; the
// public request/status calls only touch state under the mutex.
struct BluetoothContext {
    Audiox *app;
    pthread_t thread;
    bool threadStarted;
    std::atomic<int> run;
    std::mutex mutex;

    int devId;
    int agentFd;
    bool adapterReady;
    bool scanning;
    bool pairing;
    uint8_t request;
    bt::Address pairTarget;
    char message[BT_MESSAGE_MAX];
    BluetoothDevice devices[BT_MAX_DEVICES];
    size_t deviceCount;

    BluetoothContext(Audiox *context);
    ~BluetoothContext();

    int start();
    void stop();

    // RET_WARN when busy.
    int requestScan();
    // RET_ERR for a bad address, RET_WARN when busy.
    int requestPair(const char *address);
    int buildStatusJson(char *out, size_t outSize);

    void threadMain();

private:
    bool bringUpAdapter();
    void runScan();
    void runPair(const bt::Address &target);
    void handleAgentEvent(const uint8_t *packet, size_t length);
    void setMessage(const char *format, ...) __attribute__((format(printf, 2, 3)));
    BluetoothDevice *findDevice(const bt::Address &address);
    void loadPaired();
    void savePaired();

    // Pairing progress, only touched by the worker thread.
    bool pairKeyStored;
    int pairFailure;
};
