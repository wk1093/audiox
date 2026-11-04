#pragma once

#include "bluetooth/context.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>

struct ScriptBluetoothDevice {
    char address[BT_ADDRESS_TEXT]{};
    char name[BT_NAME_MAX]{};
    bool paired{false};
    bool autoConnect{false};
    bool connected{false};
    bool connecting{false};
    bool audioSource{false};
    bool audioSink{false};
};

struct ScriptBluetoothSnapshot {
    bool ready{false};
    bool scanning{false};
    bool pairing{false};
    char message[BT_MESSAGE_MAX]{};
    unsigned count{0};
    ScriptBluetoothDevice devices[BT_MAX_DEVICES]{};
};

inline bool scriptBluetoothSnapshot(Audiox *app, ScriptBluetoothSnapshot &out) {
    auto *bt = app ? app->bluetooth : nullptr;
    if (!bt) return false;
    std::lock_guard<std::mutex> lock(bt->mutex);
    out = {};
    out.ready = bt->adapterReady;
    out.scanning = bt->scanning;
    out.pairing = bt->pairing;
    snprintf(out.message, sizeof(out.message), "%s", bt->message);
    out.count = static_cast<unsigned>(std::min(bt->deviceCount, size_t{BT_MAX_DEVICES}));
    for (unsigned i = 0; i < out.count; ++i) {
        const auto &source = bt->devices[i];
        auto &device = out.devices[i];
        bt::formatAddress(source.address, device.address, sizeof(device.address));
        snprintf(device.name, sizeof(device.name), "%s", source.name);
        device.paired = source.paired;
        device.autoConnect = source.autoConnect;
        device.audioSource = source.audioSource;
        device.audioSink = source.audioSink;
        for (const auto &link : bt->linkStatus) {
            if (!link.used || !bt::sameAddress(link.address, source.address)) continue;
            device.connecting = link.connecting;
            device.connected = link.inputConnected || link.outputConnected;
        }
    }
    return true;
}

inline uint64_t scriptBluetoothHash(const ScriptBluetoothSnapshot &status) {
    uint64_t hash = 14695981039346656037ULL;
    auto byte = [&](unsigned char value) { hash = (hash ^ value) * 1099511628211ULL; };
    auto text = [&](const char *value) { while (*value) byte(static_cast<unsigned char>(*value++)); byte(0); };
    byte(status.ready);
    byte(status.scanning);
    byte(status.pairing);
    byte(status.count);
    text(status.message);
    for (unsigned i = 0; i < status.count; ++i) {
        const auto &device = status.devices[i];
        text(device.address);
        text(device.name);
        byte(device.paired);
        byte(device.autoConnect);
        byte(device.connected);
        byte(device.connecting);
        byte(device.audioSource);
        byte(device.audioSink);
    }
    return hash;
}
