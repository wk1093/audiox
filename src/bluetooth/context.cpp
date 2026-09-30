#include "bluetooth/context.hpp"

#include "defs.hpp"

#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define BT_STATE_DIR ROOT_MOUNT_POINT "/bluetooth"
#define BT_PAIRED_FILE BT_STATE_DIR "/paired.txt"
#define BT_PAIR_TIMEOUT_MS 30000ULL

#define BT_AF_BLUETOOTH 31
#define BT_PROTO_L2CAP 0
#define BT_SOL_BLUETOOTH 274
#define BT_SECURITY_OPT 4
#define BT_SECURITY_MEDIUM 2
#define BT_PSM_SDP 1

#define BT_EVT_AUTH_COMPLETE 0x06
#define BT_EVT_PIN_CODE_REQUEST 0x16
#define BT_EVT_LINK_KEY_REQUEST 0x17
#define BT_EVT_LINK_KEY_NOTIFY 0x18
#define BT_EVT_IO_CAPABILITY_REQUEST 0x31
#define BT_EVT_USER_CONFIRM_REQUEST 0x33
#define BT_EVT_USER_PASSKEY_REQUEST 0x34
#define BT_EVT_SIMPLE_PAIRING_COMPLETE 0x36

#define BT_CMD_LINK_KEY_REPLY 0x040B
#define BT_CMD_LINK_KEY_NEG_REPLY 0x040C
#define BT_CMD_PIN_CODE_REPLY 0x040D
#define BT_CMD_IO_CAPABILITY_REPLY 0x042B
#define BT_CMD_USER_CONFIRM_REPLY 0x042C
#define BT_CMD_USER_PASSKEY_NEG_REPLY 0x042F
#define BT_CMD_WRITE_SSP_MODE 0x0C56

#define BT_IO_CAP_NO_INPUT_NO_OUTPUT 0x03
#define BT_AUTH_GENERAL_BONDING 0x04

namespace {

struct L2capAddress {
    uint16_t family;
    uint16_t psm;
    uint8_t bdaddr[6];
    uint16_t cid;
    uint8_t bdaddrType;
};

struct Security {
    uint8_t level;
    uint8_t keySize;
};

static_assert(sizeof(L2capAddress) == 14, "unexpected sockaddr_l2 layout");

void *bluetoothThreadMain(void *arg) {
    static_cast<BluetoothContext *>(arg)->threadMain();
    return nullptr;
}

bool parseHexKey(const char *text, uint8_t *out) {
    for (int i = 0; i < 16; ++i) {
        char hex[3] = {text[i * 2], text[i * 2 + 1], '\0'};
        char *end = nullptr;
        out[i] = (uint8_t)strtoul(hex, &end, 16);
        if (!end || *end != '\0') {
            return false;
        }
    }
    return true;
}

} // namespace

BluetoothContext::BluetoothContext(Audiox *context)
    : app(context),
      thread(),
      threadStarted(false),
      run(0),
      devId(-1),
      agentFd(-1),
      adapterReady(false),
      scanning(false),
      pairing(false),
      request(BT_REQUEST_NONE),
      pairTarget(),
      message(),
      devices(),
      deviceCount(0),
      pairKeyStored(false),
      pairFailure(0) {
    if (app) {
        app->bluetooth = this;
    }
    loadPaired();
}

BluetoothContext::~BluetoothContext() {
    stop();
    if (app && app->bluetooth == this) {
        app->bluetooth = nullptr;
    }
}

int BluetoothContext::start() {
    if (threadStarted) {
        return RET_OK;
    }
    run.store(1, std::memory_order_release);
    if (pthread_create(&thread, nullptr, bluetoothThreadMain, this) != 0) {
        run.store(0, std::memory_order_release);
        printf("[BT] [WARN] failed to start bluetooth thread\n");
        return RET_WARN;
    }
    threadStarted = true;
    return RET_OK;
}

void BluetoothContext::stop() {
    if (!threadStarted) {
        return;
    }
    run.store(0, std::memory_order_release);
    pthread_join(thread, nullptr);
    threadStarted = false;
    if (agentFd >= 0) {
        close(agentFd);
        agentFd = -1;
    }
}

int BluetoothContext::requestScan() {
    std::lock_guard<std::mutex> lock(mutex);
    if (request != BT_REQUEST_NONE || scanning || pairing) {
        return RET_WARN;
    }
    request = BT_REQUEST_SCAN;
    scanning = true;
    snprintf(message, sizeof(message), "Scanning...");
    return RET_OK;
}

int BluetoothContext::requestPair(const char *address) {
    bt::Address target = {};
    if (!bt::parseAddress(address, &target) || bt::isZeroAddress(target)) {
        return RET_ERR;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (request != BT_REQUEST_NONE || scanning || pairing) {
        return RET_WARN;
    }
    request = BT_REQUEST_PAIR;
    pairTarget = target;
    pairing = true;
    snprintf(message, sizeof(message), "Pairing %s...", address);
    return RET_OK;
}

int BluetoothContext::buildStatusJson(char *out, size_t outSize) {
    if (!out || outSize == 0) {
        return RET_ERR;
    }
    std::lock_guard<std::mutex> lock(mutex);
    int n = snprintf(out, outSize,
                     "{\"ok\":true,\"ready\":%s,\"scanning\":%s,\"pairing\":%s,\"message\":\"%s\",\"devices\":[",
                     adapterReady ? "true" : "false",
                     scanning ? "true" : "false",
                     pairing ? "true" : "false",
                     message);
    if (n < 0 || (size_t)n >= outSize) {
        return RET_ERR;
    }
    size_t used = (size_t)n;
    for (size_t i = 0; i < deviceCount; ++i) {
        char address[BT_ADDRESS_TEXT] = {};
        bt::formatAddress(devices[i].address, address, sizeof(address));
        n = snprintf(out + used, outSize - used,
                     "%s{\"address\":\"%s\",\"name\":\"%s\",\"paired\":%s,\"classic\":%s}",
                     i > 0 ? "," : "",
                     address,
                     devices[i].name,
                     devices[i].paired ? "true" : "false",
                     devices[i].classic ? "true" : "false");
        if (n < 0 || (size_t)n >= outSize - used) {
            return RET_ERR;
        }
        used += (size_t)n;
    }
    n = snprintf(out + used, outSize - used, "]}\n");
    if (n < 0 || (size_t)n >= outSize - used) {
        return RET_ERR;
    }
    return RET_OK;
}

void BluetoothContext::setMessage(const char *format, ...) {
    std::lock_guard<std::mutex> lock(mutex);
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    for (char *c = message; *c; ++c) {
        if (*c == '"' || *c == '\\') {
            *c = '\'';
        }
    }
}

BluetoothDevice *BluetoothContext::findDevice(const bt::Address &address) {
    for (size_t i = 0; i < deviceCount; ++i) {
        if (bt::sameAddress(devices[i].address, address)) {
            return &devices[i];
        }
    }
    return nullptr;
}

void BluetoothContext::threadMain() {
    printf("[BT] [INFO] bluetooth thread started\n");
    bool firstAttempt = true;
    while (run.load(std::memory_order_acquire)) {
        uint8_t next = BT_REQUEST_NONE;
        bt::Address target = {};
        {
            std::lock_guard<std::mutex> lock(mutex);
            next = request;
            target = pairTarget;
            request = BT_REQUEST_NONE;
        }

        // Retry bring-up only when asked to do something, to keep the log quiet.
        if (!adapterReady && (firstAttempt || next != BT_REQUEST_NONE)) {
            firstAttempt = false;
            if (!bringUpAdapter()) {
                std::lock_guard<std::mutex> lock(mutex);
                scanning = false;
                pairing = false;
                snprintf(message, sizeof(message), "Bluetooth adapter unavailable");
                continue;
            }
        }

        if (next == BT_REQUEST_SCAN) {
            runScan();
            continue;
        }
        if (next == BT_REQUEST_PAIR) {
            runPair(target);
            continue;
        }

        if (agentFd < 0) {
            usleep(200000);
            continue;
        }
        struct pollfd pollFd = {agentFd, POLLIN, 0};
        if (poll(&pollFd, 1, 200) > 0) {
            uint8_t packet[300];
            ssize_t length = read(agentFd, packet, sizeof(packet));
            if (length > 0) {
                handleAgentEvent(packet, (size_t)length);
            }
        }
    }
    printf("[BT] [INFO] bluetooth thread stopped\n");
}

bool BluetoothContext::bringUpAdapter() {
    int id = bt::openAdapter();
    if (id < 0) {
        return false;
    }
    int fd = bt::openRawSocket(id);
    if (fd < 0) {
        return false;
    }

    if (bt::setLocalName(fd, "AudioX") == RET_OK) {
        printf("[BT] [INFO] local Bluetooth name set to AudioX\n");
    } else {
        printf("[BT] [WARN] could not set local Bluetooth name to AudioX\n");
    }

    const uint8_t sspEnable[] = {0x01};
    if (bt::writeCommand(fd, BT_CMD_WRITE_SSP_MODE, sspEnable, sizeof(sspEnable)) != RET_OK ||
        bt::waitForCommand(fd, BT_CMD_WRITE_SSP_MODE, 1000) != 0) {
        printf("[BT] [WARN] could not enable simple pairing; legacy PIN pairing only\n");
    }

    devId = id;
    agentFd = fd;
    std::lock_guard<std::mutex> lock(mutex);
    adapterReady = true;
    snprintf(message, sizeof(message), "Ready");
    return true;
}

void BluetoothContext::runScan() {
    static bt::DiscoveredDevice found[BT_MAX_DEVICES];
    printf("[BT] [INFO] scan started on hci%d\n", devId);
    size_t count = bt::scan(devId, found, BT_MAX_DEVICES);
    printf("[BT] [INFO] scan finished: %zu device(s)\n", count);

    std::lock_guard<std::mutex> lock(mutex);
    // Paired devices stay listed even when they are not advertising right now.
    size_t kept = 0;
    for (size_t i = 0; i < deviceCount; ++i) {
        if (devices[i].paired) {
            devices[kept++] = devices[i];
        }
    }
    deviceCount = kept;
    for (size_t i = 0; i < count; ++i) {
        BluetoothDevice *device = findDevice(found[i].address);
        if (!device) {
            if (deviceCount >= BT_MAX_DEVICES) {
                break;
            }
            device = &devices[deviceCount++];
            memset(device, 0, sizeof(*device));
            device->address = found[i].address;
        }
        if (found[i].name[0]) {
            snprintf(device->name, sizeof(device->name), "%s", found[i].name);
        }
        device->classic = device->classic || found[i].classic;
    }
    scanning = false;
    snprintf(message, sizeof(message), "Found %zu device(s)", count);
}

void BluetoothContext::runPair(const bt::Address &target) {
    char address[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(target, address, sizeof(address));
    {
        std::lock_guard<std::mutex> lock(mutex);
        BluetoothDevice *device = findDevice(target);
        if (device && !device->classic && !device->paired) {
            pairing = false;
            snprintf(message, sizeof(message), "%s is LE only; LE pairing is not supported yet", address);
            return;
        }
    }

    printf("[BT] [INFO] pairing %s\n", address);
    pairKeyStored = false;
    pairFailure = 0;

    int fd = socket(BT_AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BT_PROTO_L2CAP);
    if (fd < 0) {
        printf("[BT] [WARN] L2CAP socket failed: %s\n", strerror(errno));
        setMessage("Pairing failed: L2CAP unavailable (%s)", strerror(errno));
        std::lock_guard<std::mutex> lock(mutex);
        pairing = false;
        return;
    }

    // Asking for an encrypted link makes the kernel authenticate first, which
    // runs pairing through our agent.
    Security security = {BT_SECURITY_MEDIUM, 0};
    if (setsockopt(fd, BT_SOL_BLUETOOTH, BT_SECURITY_OPT, &security, sizeof(security)) < 0) {
        printf("[BT] [WARN] BT_SECURITY failed: %s\n", strerror(errno));
    }

    L2capAddress remote = {};
    remote.family = BT_AF_BLUETOOTH;
    remote.psm = BT_PSM_SDP;
    memcpy(remote.bdaddr, target.b, sizeof(remote.bdaddr));
    bool connecting = true;
    bool connected = false;
    int connectError = 0;
    if (connect(fd, (struct sockaddr *)&remote, sizeof(remote)) == 0) {
        connecting = false;
        connected = true;
    } else if (errno != EINPROGRESS) {
        connecting = false;
        connectError = errno;
    }

    const uint64_t deadline = bt::monotonicMs() + BT_PAIR_TIMEOUT_MS;
    while (connecting && pairFailure == 0 && run.load(std::memory_order_acquire) &&
           bt::monotonicMs() < deadline) {
        struct pollfd fds[2] = {
            {agentFd, POLLIN, 0},
            {fd, POLLOUT, 0},
        };
        uint64_t remaining = deadline - bt::monotonicMs();
        int timeout = remaining > 500ULL ? 500 : (int)remaining;
        if (poll(fds, 2, timeout) <= 0) {
            continue;
        }
        if (fds[0].revents & POLLIN) {
            uint8_t packet[300];
            ssize_t length = read(agentFd, packet, sizeof(packet));
            if (length > 0) {
                handleAgentEvent(packet, (size_t)length);
            }
        }
        if (fds[1].revents & (POLLOUT | POLLERR | POLLHUP)) {
            int error = 0;
            socklen_t errorLength = sizeof(error);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength);
            connecting = false;
            connected = error == 0;
            connectError = error;
        }
    }
    close(fd);

    bool paired = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        BluetoothDevice *device = findDevice(target);
        paired = pairKeyStored || (connected && device && device->paired);
    }

    if (paired) {
        printf("[BT] [INFO] paired %s\n", address);
        setMessage("Paired %s", address);
    } else if (pairFailure != 0) {
        printf("[BT] [WARN] pairing %s failed: HCI status 0x%02x\n", address, pairFailure);
        setMessage("Pairing %s failed (HCI status 0x%02x)", address, pairFailure);
    } else if (connecting) {
        printf("[BT] [WARN] pairing %s timed out\n", address);
        setMessage("Pairing %s timed out", address);
    } else {
        printf("[BT] [WARN] pairing %s failed: %s\n", address, strerror(connectError));
        setMessage("Pairing %s failed: %s", address, strerror(connectError));
    }
    std::lock_guard<std::mutex> lock(mutex);
    pairing = false;
}

void BluetoothContext::handleAgentEvent(const uint8_t *packet, size_t length) {
    if (length < 3 || packet[0] != 0x04) {
        return;
    }
    const uint8_t event = packet[1];
    const uint8_t *params = packet + 3;
    const size_t paramLength = length - 3;

    bt::Address address = {};
    if (event != BT_EVT_AUTH_COMPLETE && event != BT_EVT_SIMPLE_PAIRING_COMPLETE) {
        if (paramLength < 6) {
            return;
        }
        memcpy(address.b, params, sizeof(address.b));
    }
    char addressText[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(address, addressText, sizeof(addressText));

    switch (event) {
    case BT_EVT_LINK_KEY_REQUEST: {
        uint8_t reply[22] = {};
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(mutex);
            BluetoothDevice *device = findDevice(address);
            if (device && device->paired) {
                memcpy(reply, address.b, 6);
                memcpy(reply + 6, device->linkKey, 16);
                known = true;
            }
        }
        if (known) {
            bt::writeCommand(agentFd, BT_CMD_LINK_KEY_REPLY, reply, sizeof(reply));
        } else {
            bt::writeCommand(agentFd, BT_CMD_LINK_KEY_NEG_REPLY, address.b, 6);
        }
        printf("[BT] [INFO] link key request from %s: %s\n", addressText, known ? "known" : "unknown");
        break;
    }
    case BT_EVT_IO_CAPABILITY_REQUEST: {
        const uint8_t reply[] = {
            address.b[0], address.b[1], address.b[2], address.b[3], address.b[4], address.b[5],
            BT_IO_CAP_NO_INPUT_NO_OUTPUT, 0x00, BT_AUTH_GENERAL_BONDING,
        };
        bt::writeCommand(agentFd, BT_CMD_IO_CAPABILITY_REPLY, reply, sizeof(reply));
        printf("[BT] [INFO] IO capability request from %s\n", addressText);
        break;
    }
    case BT_EVT_USER_CONFIRM_REQUEST:
        bt::writeCommand(agentFd, BT_CMD_USER_CONFIRM_REPLY, address.b, 6);
        printf("[BT] [INFO] confirmed pairing with %s\n", addressText);
        break;
    case BT_EVT_USER_PASSKEY_REQUEST:
        bt::writeCommand(agentFd, BT_CMD_USER_PASSKEY_NEG_REPLY, address.b, 6);
        printf("[BT] [WARN] %s asked for a passkey; rejected\n", addressText);
        break;
    case BT_EVT_PIN_CODE_REQUEST: {
        uint8_t reply[23] = {};
        memcpy(reply, address.b, 6);
        reply[6] = 4;
        memcpy(reply + 7, "0000", 4);
        bt::writeCommand(agentFd, BT_CMD_PIN_CODE_REPLY, reply, sizeof(reply));
        printf("[BT] [INFO] sent PIN 0000 to %s\n", addressText);
        break;
    }
    case BT_EVT_LINK_KEY_NOTIFY: {
        if (paramLength < 23) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            BluetoothDevice *device = findDevice(address);
            if (!device && deviceCount < BT_MAX_DEVICES) {
                device = &devices[deviceCount++];
                memset(device, 0, sizeof(*device));
                device->address = address;
                device->classic = true;
            }
            if (device) {
                memcpy(device->linkKey, params + 6, 16);
                device->linkKeyType = params[22];
                device->paired = true;
            }
            if (pairing && bt::sameAddress(address, pairTarget)) {
                pairKeyStored = true;
            }
        }
        printf("[BT] [INFO] stored link key for %s (type %u)\n", addressText, (unsigned)params[22]);
        savePaired();
        break;
    }
    case BT_EVT_SIMPLE_PAIRING_COMPLETE:
        if (paramLength >= 7 && params[0] != 0) {
            memcpy(address.b, params + 1, 6);
            std::lock_guard<std::mutex> lock(mutex);
            if (pairing && bt::sameAddress(address, pairTarget)) {
                pairFailure = params[0];
            }
        }
        break;
    case BT_EVT_AUTH_COMPLETE:
        if (paramLength >= 1 && params[0] != 0) {
            std::lock_guard<std::mutex> lock(mutex);
            if (pairing) {
                pairFailure = params[0];
            }
        }
        break;
    default:
        break;
    }
}

// Format: one device per line, "ADDRESS KEYHEX KEYTYPE NAME".
void BluetoothContext::loadPaired() {
    FILE *fp = fopen(BT_PAIRED_FILE, "r");
    if (!fp) {
        return;
    }
    char line[BT_NAME_MAX + 64];
    std::lock_guard<std::mutex> lock(mutex);
    while (fgets(line, sizeof(line), fp) && deviceCount < BT_MAX_DEVICES) {
        line[strcspn(line, "\r\n")] = '\0';
        char addressText[BT_ADDRESS_TEXT] = {};
        char keyText[33] = {};
        unsigned keyType = 0;
        int nameOffset = 0;
        if (sscanf(line, "%17s %32s %u %n", addressText, keyText, &keyType, &nameOffset) < 3) {
            continue;
        }
        BluetoothDevice device = {};
        if (!bt::parseAddress(addressText, &device.address) || strlen(keyText) != 32 ||
            !parseHexKey(keyText, device.linkKey)) {
            continue;
        }
        device.linkKeyType = (uint8_t)keyType;
        device.paired = true;
        device.classic = true;
        if (nameOffset > 0) {
            snprintf(device.name, sizeof(device.name), "%s", line + nameOffset);
        }
        devices[deviceCount++] = device;
    }
    fclose(fp);
    printf("[BT] [INFO] loaded %zu paired device(s)\n", deviceCount);
}

void BluetoothContext::savePaired() {
    mkdir(BT_STATE_DIR, 0755);
    const char *tmpPath = BT_PAIRED_FILE ".tmp";
    FILE *fp = fopen(tmpPath, "w");
    if (!fp) {
        printf("[BT] [WARN] cannot write %s: %s\n", tmpPath, strerror(errno));
        return;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (size_t i = 0; i < deviceCount; ++i) {
            const BluetoothDevice &device = devices[i];
            if (!device.paired) {
                continue;
            }
            char addressText[BT_ADDRESS_TEXT] = {};
            bt::formatAddress(device.address, addressText, sizeof(addressText));
            fprintf(fp, "%s ", addressText);
            for (uint8_t byte : device.linkKey) {
                fprintf(fp, "%02x", byte);
            }
            fprintf(fp, " %u %s\n", (unsigned)device.linkKeyType, device.name);
        }
    }
    fflush(fp);
    fsync(fileno(fp));
    fclose(fp);
    if (rename(tmpPath, BT_PAIRED_FILE) != 0) {
        printf("[BT] [WARN] cannot replace %s: %s\n", BT_PAIRED_FILE, strerror(errno));
    }
}
