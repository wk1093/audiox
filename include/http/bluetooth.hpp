#pragma once

#include <cstddef>

int bluetoothListDevices(char *out, size_t outSize, bool scan);
int bluetoothPairDevice(const char *address);
int bluetoothConnectDevice(const char *address);