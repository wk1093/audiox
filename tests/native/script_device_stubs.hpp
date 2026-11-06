#pragma once

#include "bluetooth/context.hpp"
#include <cerrno>
#include <cstdio>

inline unsigned scriptBluetoothRequests = 0;
inline unsigned scriptRestartRequests = 0;
inline int scriptBluetoothResult = RET_WARN;

BluetoothContext::BluetoothContext(Audiox *context) : app(context) {
    adapterReady = true;
    scanning = pairing = false;
    message[0] = '\0';
    deviceCount = 0;
    for (auto &link : linkStatus) link = {};
    if (app) app->bluetooth = this;
}
BluetoothContext::~BluetoothContext() {
    if (app && app->bluetooth == this) app->bluetooth = nullptr;
}

int BluetoothContext::requestScan() { ++scriptBluetoothRequests; return scriptBluetoothResult; }
int BluetoothContext::requestPair(const char *) { ++scriptBluetoothRequests; return scriptBluetoothResult; }
int BluetoothContext::requestConnect(const char *) { ++scriptBluetoothRequests; return scriptBluetoothResult; }
int BluetoothContext::requestDisconnect(const char *) { ++scriptBluetoothRequests; return scriptBluetoothResult; }
int BluetoothContext::requestUnpair(const char *) { ++scriptBluetoothRequests; return scriptBluetoothResult; }
int BluetoothContext::requestAutoConnect(const char *, bool) { ++scriptBluetoothRequests; return scriptBluetoothResult; }
void flushLogs() { fflush(nullptr); }
extern "C" int reboot(int) noexcept {
    ++scriptRestartRequests;
    errno = EPERM;
    return -1;
}
