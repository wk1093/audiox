#include "bluetooth/context.hpp"

#include "audio/context.hpp"
#include "defs.hpp"

#include <errno.h>
#include <endian.h>
#include <poll.h>
#include <sched.h>
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
#define BT_SOL_L2CAP 6
#define BT_L2CAP_OPTIONS 1
#define BT_SECURITY_OPT 4
#define BT_SECURITY_MEDIUM 2
#define BT_PSM_SDP 1
#define BT_PSM_AVDTP 0x0019
#define BT_A2DP_DEFAULT_MEDIA_MTU 672U
#define BT_A2DP_REQUESTED_MEDIA_MTU 1024U
#define BT_A2DP_MAX_MEDIA_MTU 1024U
#define BT_A2DP_TARGET_BITPOOL 20U
#define BT_RTP_SBC_HEADER_SIZE 13U
#define BT_MAX_SBC_FRAMES_PER_PACKET 15U

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
#define BT_CMD_DELETE_STORED_LINK_KEY 0x0C12
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

struct L2capOptions {
    uint16_t omtu;
    uint16_t imtu;
    uint16_t flushTo;
    uint8_t mode;
    uint8_t fcs;
    uint8_t maxTx;
    uint16_t txWindowSize;
};

static_assert(sizeof(L2capAddress) == 14, "unexpected sockaddr_l2 layout");
static_assert(sizeof(L2capOptions) == 12, "unexpected l2cap_options layout");

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

void appendBytes(uint8_t *out, size_t capacity, size_t *length,
                 const uint8_t *data, size_t dataLength) {
    if (dataLength > capacity - *length) {
        return;
    }
    memcpy(out + *length, data, dataLength);
    *length += dataLength;
}

void appendSequence(uint8_t *out, size_t capacity, size_t *length,
                    const uint8_t *data, size_t dataLength) {
    if (dataLength > 255 || dataLength + 2 > capacity - *length) {
        return;
    }
    out[(*length)++] = 0x35;
    out[(*length)++] = (uint8_t)dataLength;
    appendBytes(out, capacity, length, data, dataLength);
}

void appendAttribute(uint8_t *out, size_t capacity, size_t *length,
                     uint16_t id, const uint8_t *value, size_t valueLength) {
    if (valueLength + 3 > capacity - *length) {
        return;
    }
    out[(*length)++] = 0x09;
    out[(*length)++] = (uint8_t)(id >> 8);
    out[(*length)++] = (uint8_t)id;
    appendBytes(out, capacity, length, value, valueLength);
}

size_t makeA2dpServiceRecord(uint8_t *out, size_t capacity) {
    uint8_t attributes[256] = {};
    size_t attributesLength = 0;

    const uint8_t serviceClasses[] = {0x19, 0x11, 0x0B};
    uint8_t value[64] = {};
    size_t valueLength = 0;
    appendSequence(value, sizeof(value), &valueLength, serviceClasses, sizeof(serviceClasses));
    appendAttribute(attributes, sizeof(attributes), &attributesLength, 0x0001, value, valueLength);

    uint8_t protocolDescriptors[64] = {};
    size_t protocolLength = 0;
    uint8_t l2capDescriptor[] = {0x19, 0x01, 0x00, 0x09, 0x00, BT_PSM_AVDTP};
    uint8_t avdtpDescriptor[] = {0x19, 0x00, 0x19, 0x09, 0x01, 0x03};
    appendSequence(protocolDescriptors, sizeof(protocolDescriptors), &protocolLength,
                   l2capDescriptor, sizeof(l2capDescriptor));
    appendSequence(protocolDescriptors, sizeof(protocolDescriptors), &protocolLength,
                   avdtpDescriptor, sizeof(avdtpDescriptor));
    valueLength = 0;
    appendSequence(value, sizeof(value), &valueLength, protocolDescriptors, protocolLength);
    appendAttribute(attributes, sizeof(attributes), &attributesLength, 0x0004, value, valueLength);

    const uint8_t browseGroup[] = {0x19, 0x10, 0x02};
    valueLength = 0;
    appendSequence(value, sizeof(value), &valueLength, browseGroup, sizeof(browseGroup));
    appendAttribute(attributes, sizeof(attributes), &attributesLength, 0x0005, value, valueLength);

    uint8_t profileDescriptor[16] = {0x19, 0x11, 0x0D, 0x09, 0x01, 0x03};
    valueLength = 0;
    appendSequence(value, sizeof(value), &valueLength, profileDescriptor, sizeof(profileDescriptor));
    valueLength = 0;
    appendSequence(value, sizeof(value), &valueLength, profileDescriptor, 6);
    uint8_t profileList[24] = {};
    size_t profileListLength = 0;
    appendBytes(profileList, sizeof(profileList), &profileListLength, value, valueLength);
    valueLength = 0;
    appendSequence(value, sizeof(value), &valueLength, profileList, profileListLength);
    appendAttribute(attributes, sizeof(attributes), &attributesLength, 0x0009, value, valueLength);

    const uint8_t serviceName[] = {0x25, 0x06, 'A', 'u', 'd', 'i', 'o', 'X'};
    appendAttribute(attributes, sizeof(attributes), &attributesLength, 0x0100,
                    serviceName, sizeof(serviceName));

    if (attributesLength + 4 > capacity) {
        return 0;
    }
    size_t recordLength = 0;
    appendSequence(out, capacity, &recordLength, attributes, attributesLength);
    return recordLength;
}

uint16_t readBe16(const uint8_t *data) {
    return (uint16_t)(((uint16_t)data[0] << 8) | data[1]);
}

void writeBe16(uint8_t *data, uint16_t value) {
    data[0] = (uint8_t)(value >> 8);
    data[1] = (uint8_t)value;
}

} // namespace

BluetoothContext::BluetoothContext(Audiox *context)
    : app(context),
      thread(),
      threadStarted(false),
      run(0),
      devId(-1),
      agentFd(-1),
            sdpListenFd(-1),
            avdtpListenFd(-1),
            signalingFd(-1),
            mediaFd(-1),
            outputSignalingFd(-1),
            outputMediaFd(-1),
      adapterReady(false),
      scanning(false),
      pairing(false),
      request(BT_REQUEST_NONE),
      pairTarget(),
      message(),
      devices(),
      deviceCount(0),
            sbcDecoder(),
            sbcDecoderInitialized(false),
            sbcChannels(0),
            avdtpConfigured(false),
            avdtpOpened(false),
            avdtpStreaming(false),
            sbcConfiguration(),
            sbcFragment(),
            sbcFragmentSize(0),
            sbcFragmentActive(false),
            sbcEncoder(),
            sbcEncoderInitialized(false),
            outputSeid(0),
            outputTransaction(0),
            outputPendingSignal(0),
            rtpSequence(0),
            rtpTimestamp(0),
            outputConnecting(false),
            outputConnectingStatus(0),
            outputMediaConnecting(false),
            outputConfigured(false),
            outputOpened(false),
            outputConnected(0),
            outputPacket(),
            outputPacketLength(0),
            outputStatsNextMs(0),
            outputPacketsSinceStats(0),
            outputDroppedAtStats(0),
            outputFramesPerPacket(0),
            outputMediaMtu(BT_A2DP_DEFAULT_MEDIA_MTU),
            outputSendWouldBlock(0),
            outputWouldBlockAtStats(0),
            outputRealtimeScheduling(false),
            outputTarget(),
      pairKeyStored(false),
      pairFailure(0) {
    if (app) {
        app->bluetooth = this;
    }
    loadPaired();
}

BluetoothContext::~BluetoothContext() {
    stop();
    if (sbcDecoderInitialized) {
        sbc_finish(&sbcDecoder);
        sbcDecoderInitialized = false;
    }
    if (sbcEncoderInitialized) {
        sbc_finish(&sbcEncoder);
        sbcEncoderInitialized = false;
    }
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
    disconnectOutput(nullptr);
    if (agentFd >= 0) {
        close(agentFd);
        agentFd = -1;
    }
    closeA2dpSockets();
}

int BluetoothContext::requestScan() {
    std::lock_guard<std::mutex> lock(mutex);
    if (request != BT_REQUEST_NONE || scanning || pairing ||
        outputConnectingStatus.load(std::memory_order_relaxed) ||
        outputConnected.load(std::memory_order_relaxed)) {
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
    if (request != BT_REQUEST_NONE || scanning || pairing ||
        outputConnectingStatus.load(std::memory_order_relaxed) ||
        outputConnected.load(std::memory_order_relaxed)) {
        return RET_WARN;
    }
    request = BT_REQUEST_PAIR;
    pairTarget = target;
    pairing = true;
    snprintf(message, sizeof(message), "Pairing %s...", address);
    return RET_OK;
}

int BluetoothContext::requestConnect(const char *address) {
    bt::Address target = {};
    if (!bt::parseAddress(address, &target) || bt::isZeroAddress(target)) {
        return RET_ERR;
    }
    std::lock_guard<std::mutex> lock(mutex);
    BluetoothDevice *device = findDevice(target);
    if (!device || !device->paired || !device->classic) {
        return RET_ERR;
    }
    if (request != BT_REQUEST_NONE || scanning || pairing ||
        outputConnectingStatus.load(std::memory_order_relaxed) ||
        outputConnected.load(std::memory_order_relaxed)) {
        return RET_WARN;
    }
    outputTarget = target;
    pairTarget = target;
    request = BT_REQUEST_CONNECT;
    outputConnectingStatus.store(1, std::memory_order_release);
    snprintf(message, sizeof(message), "Connecting to %s...", device->name[0] ? device->name : address);
    return RET_OK;
}

int BluetoothContext::requestDisconnect() {
    std::lock_guard<std::mutex> lock(mutex);
    if (request != BT_REQUEST_NONE || scanning || pairing ||
        outputConnectingStatus.load(std::memory_order_relaxed) == 1) {
        return RET_WARN;
    }
    if (!outputConnected.load(std::memory_order_relaxed)) {
        return RET_WARN;
    }
    request = BT_REQUEST_DISCONNECT;
    snprintf(message, sizeof(message), "Disconnecting Bluetooth output...");
    return RET_OK;
}

int BluetoothContext::requestUnpair(const char *address) {
    bt::Address target = {};
    if (!bt::parseAddress(address, &target) || bt::isZeroAddress(target)) {
        return RET_ERR;
    }
    std::lock_guard<std::mutex> lock(mutex);
    BluetoothDevice *device = findDevice(target);
    if (!device || !device->paired) {
        return RET_ERR;
    }
    if (request != BT_REQUEST_NONE || scanning || pairing ||
        outputConnectingStatus.load(std::memory_order_relaxed) == 1) {
        return RET_WARN;
    }
    pairTarget = target;
    request = BT_REQUEST_UNPAIR;
    snprintf(message, sizeof(message), "Forgetting %s...", device->name[0] ? device->name : address);
    return RET_OK;
}

int BluetoothContext::buildStatusJson(char *out, size_t outSize) {
    if (!out || outSize == 0) {
        return RET_ERR;
    }
    std::lock_guard<std::mutex> lock(mutex);
    int n = snprintf(out, outSize,
                     "{\"ok\":true,\"ready\":%s,\"scanning\":%s,\"pairing\":%s,\"connecting\":%s,\"connected\":%s,\"message\":\"%s\",\"devices\":[",
                     adapterReady ? "true" : "false",
                     scanning ? "true" : "false",
                     pairing ? "true" : "false",
                     outputConnectingStatus.load(std::memory_order_relaxed) ? "true" : "false",
                     outputConnected.load(std::memory_order_relaxed) ? "true" : "false",
                     message);
    if (n < 0 || (size_t)n >= outSize) {
        return RET_ERR;
    }
    size_t used = (size_t)n;
    for (size_t i = 0; i < deviceCount; ++i) {
        char address[BT_ADDRESS_TEXT] = {};
        bt::formatAddress(devices[i].address, address, sizeof(address));
        n = snprintf(out + used, outSize - used,
                     "%s{\"address\":\"%s\",\"name\":\"%s\",\"paired\":%s,\"classic\":%s,\"connected\":%s}",
                     i > 0 ? "," : "",
                     address,
                     devices[i].name,
                     devices[i].paired ? "true" : "false",
                     devices[i].classic ? "true" : "false",
                     outputConnected.load(std::memory_order_relaxed) &&
                         bt::sameAddress(outputTarget, devices[i].address) ? "true" : "false");
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

bool BluetoothContext::getOutputDeviceName(char *out, size_t outSize) const {
    if (!out || outSize == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const BluetoothDevice *selected = nullptr;
    for (size_t i = 0; i < deviceCount; ++i) {
        if (devices[i].paired && devices[i].classic && bt::sameAddress(outputTarget, devices[i].address)) {
            selected = &devices[i];
            break;
        }
        if (!selected && devices[i].paired && devices[i].classic) {
            selected = &devices[i];
        }
    }
    if (!selected) {
        return false;
    }
    snprintf(out, outSize, "%s", selected->name[0] ? selected->name : "Paired Bluetooth device");
    return true;
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

bool BluetoothContext::configureSbcDecoder(const uint8_t configuration[4]) {
    if (!configuration) {
        return false;
    }
    if (sbcDecoderInitialized) {
        sbc_finish(&sbcDecoder);
        sbcDecoderInitialized = false;
    }
    if (sbc_init_a2dp(&sbcDecoder, 0, configuration, 4) < 0) {
        printf("[BT] [WARN] SBC decoder rejected negotiated configuration\n");
        return false;
    }
    sbcDecoderInitialized = true;
    sbcChannels = sbcDecoder.mode == SBC_MODE_MONO ? 1 : 2;
    printf("[BT] [INFO] SBC decoder ready: %u Hz, %u channel(s), bitpool %u\n",
           sbcDecoder.frequency == SBC_FREQ_48000 ? 48000U :
           sbcDecoder.frequency == SBC_FREQ_44100 ? 44100U :
           sbcDecoder.frequency == SBC_FREQ_32000 ? 32000U : 16000U,
           (unsigned)sbcChannels,
           (unsigned)sbcDecoder.bitpool);
    return true;
}

uint32_t BluetoothContext::decodeSbcPayload(const uint8_t *payload, size_t length) {
    if (!sbcDecoderInitialized || !app || !app->audio || !payload || length < 2) {
        return 0;
    }
    const uint8_t packetHeader = payload[0];
    if ((packetHeader & 0x10U) != 0) {
        return 0;
    }
    const bool fragmented = (packetHeader & 0x80U) != 0;
    const bool start = (packetHeader & 0x40U) != 0;
    const bool last = (packetHeader & 0x20U) != 0;
    if (fragmented) {
        const uint8_t *fragment = payload + 1;
        size_t fragmentLength = length - 1;
        if (start) {
            sbcFragmentSize = 0;
            sbcFragmentActive = true;
        } else if (!sbcFragmentActive) {
            return 0;
        }
        if (fragmentLength > sizeof(sbcFragment) - sbcFragmentSize) {
            sbcFragmentActive = false;
            sbcFragmentSize = 0;
            return 0;
        }
        memcpy(sbcFragment + sbcFragmentSize, fragment, fragmentLength);
        sbcFragmentSize += fragmentLength;
        if (!last) {
            return 0;
        }
        uint8_t completeFrame[sizeof(sbcFragment) + 1];
        completeFrame[0] = 1;
        memcpy(completeFrame + 1, sbcFragment, sbcFragmentSize);
        size_t completeLength = sbcFragmentSize + 1;
        sbcFragmentActive = false;
        sbcFragmentSize = 0;
        return decodeSbcPayload(completeFrame, completeLength);
    }
    if (start || last) {
        return 0;
    }
    uint8_t frameCount = packetHeader & 0x0FU;
    if (frameCount == 0) {
        return 0;
    }

    const uint8_t *frame = payload + 1;
    size_t remaining = length - 1;
    uint32_t totalFrames = 0;
    int16_t decoded[16U * 8U * 2U];
    int16_t stereo[16U * 8U * 2U];
    for (uint8_t index = 0; index < frameCount && remaining > 0; ++index) {
        size_t decodedBytes = 0;
        ssize_t consumed = sbc_decode(&sbcDecoder,
                                     frame,
                                     remaining,
                                     decoded,
                                     sizeof(decoded),
                                     &decodedBytes);
        if (consumed <= 0 || (size_t)consumed > remaining ||
            decodedBytes == 0 || decodedBytes > sizeof(decoded)) {
            break;
        }
        const uint32_t channels = sbcChannels;
        const uint32_t samples = (uint32_t)(decodedBytes / (sizeof(int16_t) * channels));
        if (samples > 16U * 8U) {
            break;
        }
        for (uint32_t sample = 0; sample < samples; ++sample) {
            stereo[sample * 2U] = decoded[sample * channels];
            stereo[sample * 2U + 1U] = channels == 2 ? decoded[sample * 2U + 1U] : decoded[sample];
        }
        totalFrames += app->audio->pushBluetoothPcm(stereo, samples);
        frame += (size_t)consumed;
        remaining -= (size_t)consumed;
    }
    return totalFrames;
}

void BluetoothContext::threadMain() {
    printf("[BT] [INFO] bluetooth thread started\n");
    printf("[BT] [INFO] A2DP tuning: bitpool_cap=%u requested_media_mtu=%u media_mtu_cap=%u max_frames_per_packet=%u\n",
           BT_A2DP_TARGET_BITPOOL,
           BT_A2DP_REQUESTED_MEDIA_MTU,
           BT_A2DP_MAX_MEDIA_MTU,
           BT_MAX_SBC_FRAMES_PER_PACKET);
    bool firstAttempt = true;
    while (run.load(std::memory_order_acquire)) {
        const bool wantOutputRealtime = outputConnected.load(std::memory_order_acquire) != 0;
        if (wantOutputRealtime != outputRealtimeScheduling) {
            sched_param priority = {};
            int policy = wantOutputRealtime ? SCHED_FIFO : SCHED_OTHER;
            priority.sched_priority = wantOutputRealtime ? 98 : 0;
            int rc = pthread_setschedparam(pthread_self(), policy, &priority);
            if (rc == 0) {
                outputRealtimeScheduling = wantOutputRealtime;
                printf("[BT] [INFO] Bluetooth worker scheduler policy=%d priority=%d\n",
                       policy,
                       priority.sched_priority);
            } else {
                printf("[BT] [WARN] cannot set Bluetooth worker scheduler policy=%d priority=%d: %s\n",
                       policy,
                       priority.sched_priority,
                       strerror(rc));
                outputRealtimeScheduling = wantOutputRealtime;
            }
        }
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
        if (next == BT_REQUEST_CONNECT) {
            if (!beginOutputConnect(target)) {
                disconnectOutput("could not start A2DP connection");
            }
        }
        if (next == BT_REQUEST_DISCONNECT) {
            disconnectOutput("disconnected by user");
        }
        if (next == BT_REQUEST_UNPAIR) {
            runUnpair(target);
        }

        pumpOutputAudio();
        struct pollfd pollFds[7] = {
            {agentFd, POLLIN, 0},
            {sdpListenFd, POLLIN, 0},
            {avdtpListenFd, POLLIN, 0},
            {signalingFd, POLLIN, 0},
            {mediaFd, POLLIN, 0},
            {outputSignalingFd, (short)(outputConnecting ? POLLOUT : POLLIN), 0},
            {outputMediaFd, (short)((outputMediaConnecting || outputPacketLength > 0) ? POLLOUT : 0), 0},
        };
        int pollTimeout = outputConnected.load(std::memory_order_relaxed) ? 2 : 200;
        if (poll(pollFds, 7, pollTimeout) > 0) {
            if (agentFd >= 0 && (pollFds[0].revents & POLLIN)) {
            uint8_t packet[300];
            ssize_t length = read(agentFd, packet, sizeof(packet));
            if (length > 0) {
                handleAgentEvent(packet, (size_t)length);
            }
            }
            if (sdpListenFd >= 0 && (pollFds[1].revents & POLLIN)) {
                acceptSdpRequest();
            }
            if (avdtpListenFd >= 0 && (pollFds[2].revents & POLLIN)) {
                acceptA2dpChannel();
            }
            if (signalingFd >= 0 && (pollFds[3].revents & POLLIN)) {
                uint8_t packet[1024];
                ssize_t length = read(signalingFd, packet, sizeof(packet));
                if (length > 0) {
                    handleAvdtpPacket(packet, (size_t)length);
                } else if (length == 0) {
                    close(signalingFd);
                    signalingFd = -1;
                    if (mediaFd >= 0) {
                        close(mediaFd);
                        mediaFd = -1;
                    }
                    resetA2dpStream();
                }
            }
            if (mediaFd >= 0 && (pollFds[4].revents & POLLIN)) {
                uint8_t packet[2048];
                ssize_t length = read(mediaFd, packet, sizeof(packet));
                if (length > 0 && avdtpStreaming) {
                    handleA2dpMedia(packet, (size_t)length);
                } else if (length == 0) {
                    close(mediaFd);
                    mediaFd = -1;
                    avdtpStreaming = false;
                }
            }
            if (outputSignalingFd >= 0 && outputConnecting &&
                (pollFds[5].revents & (POLLOUT | POLLERR | POLLHUP))) {
                int error = 0;
                socklen_t errorLength = sizeof(error);
                if (getsockopt(outputSignalingFd, SOL_SOCKET, SO_ERROR, &error, &errorLength) < 0 || error != 0) {
                    disconnectOutput(strerror(error ? error : errno));
                } else {
                    outputConnecting = false;
                    sendOutputCommand(0x01, nullptr, 0);
                }
            } else if (outputSignalingFd >= 0 && !outputConnecting &&
                       (pollFds[5].revents & (POLLIN | POLLERR | POLLHUP))) {
                handleOutputSignaling();
            }
            if (outputMediaFd >= 0 && outputMediaConnecting &&
                (pollFds[6].revents & (POLLOUT | POLLERR | POLLHUP))) {
                int error = 0;
                socklen_t errorLength = sizeof(error);
                if (getsockopt(outputMediaFd, SOL_SOCKET, SO_ERROR, &error, &errorLength) < 0 || error != 0) {
                    disconnectOutput(strerror(error ? error : errno));
                } else {
                    outputMediaConnecting = false;
                    if (!readOutputMediaMtu()) {
                        disconnectOutput("could not read negotiated L2CAP media MTU");
                        continue;
                    }
                    const uint8_t seid = (uint8_t)(outputSeid << 2);
                    sendOutputCommand(0x07, &seid, 1);
                }
            } else if (outputMediaFd >= 0 && outputPacketLength > 0 &&
                       (pollFds[6].revents & POLLOUT)) {
                handleOutputMediaWritable();
            }
        }
        pumpOutputAudio();
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
    if (!openA2dpListeners()) {
        printf("[BT] [WARN] A2DP listeners unavailable; discovery and pairing remain enabled\n");
    }
    std::lock_guard<std::mutex> lock(mutex);
    adapterReady = true;
    snprintf(message, sizeof(message), "Ready");
    return true;
}

bool BluetoothContext::openA2dpListeners() {
    auto createListener = [](uint16_t psm) {
        int fd = socket(BT_AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BT_PROTO_L2CAP);
        if (fd < 0) {
            return -1;
        }
        L2capAddress local = {};
        local.family = BT_AF_BLUETOOTH;
        local.psm = htole16(psm);
        if (psm == BT_PSM_AVDTP) {
            Security security = {BT_SECURITY_MEDIUM, 0};
            if (setsockopt(fd, BT_SOL_BLUETOOTH, BT_SECURITY_OPT,
                           &security, sizeof(security)) < 0) {
                printf("[BT] [WARN] cannot require authenticated AVDTP links: %s\n", strerror(errno));
            }
        }
        if (bind(fd, (struct sockaddr *)&local, sizeof(local)) < 0 || listen(fd, 2) < 0) {
            printf("[BT] [WARN] cannot listen on L2CAP PSM 0x%04x: %s\n", psm, strerror(errno));
            close(fd);
            return -1;
        }
        return fd;
    };

    sdpListenFd = createListener(BT_PSM_SDP);
    avdtpListenFd = createListener(BT_PSM_AVDTP);
    if (sdpListenFd < 0 || avdtpListenFd < 0) {
        closeA2dpSockets();
        return false;
    }
    printf("[BT] [INFO] A2DP SDP and AVDTP listeners ready\n");
    return true;
}

void BluetoothContext::closeA2dpSockets() {
    int *fds[] = {&sdpListenFd, &avdtpListenFd, &signalingFd, &mediaFd};
    for (int *fd : fds) {
        if (*fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
    resetA2dpStream();
}

void BluetoothContext::acceptSdpRequest() {
    int fd = accept4(sdpListenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        return;
    }
    handleSdpRequest(fd);
    close(fd);
}

void BluetoothContext::acceptA2dpChannel() {
    int fd = accept4(avdtpListenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        return;
    }
    if (signalingFd < 0) {
        signalingFd = fd;
        avdtpConfigured = false;
        avdtpOpened = false;
        avdtpStreaming = false;
        printf("[BT] [INFO] AVDTP signaling channel connected\n");
    } else if (avdtpOpened && mediaFd < 0) {
        mediaFd = fd;
        printf("[BT] [INFO] A2DP media channel connected\n");
    } else {
        close(fd);
    }
}

void BluetoothContext::handleSdpRequest(int fd) {
    uint8_t requestPacket[1024] = {};
    ssize_t length = read(fd, requestPacket, sizeof(requestPacket));
    if (length < 5) {
        return;
    }
    const uint8_t pdu = requestPacket[0];
    const uint16_t transaction = readBe16(requestPacket + 1);
    const uint16_t parameterLength = readBe16(requestPacket + 3);
    if ((size_t)length < (size_t)parameterLength + 5U) {
        return;
    }

    uint8_t record[256] = {};
    size_t recordLength = makeA2dpServiceRecord(record, sizeof(record));
    uint8_t parameters[512] = {};
    size_t responseLength = 0;
    uint8_t responsePdu = (uint8_t)(pdu + 1);

    if (pdu == 0x06 && parameterLength >= 5) {
        uint8_t outer[300] = {};
        size_t outerLength = 0;
        appendSequence(outer, sizeof(outer), &outerLength, record, recordLength);
        writeBe16(parameters, (uint16_t)outerLength);
        memcpy(parameters + 2, outer, outerLength);
        parameters[2 + outerLength] = 0;
        responseLength = outerLength + 3;
    } else if (pdu == 0x02 && parameterLength >= 5) {
        writeBe16(parameters, 1);
        writeBe16(parameters + 2, 1);
        parameters[4] = 0x00;
        parameters[5] = 0x01;
        parameters[6] = 0x00;
        parameters[7] = 0x00;
        parameters[8] = 0;
        responseLength = 9;
    } else if (pdu == 0x04 && parameterLength >= 6) {
        writeBe16(parameters, (uint16_t)recordLength);
        memcpy(parameters + 2, record, recordLength);
        parameters[2 + recordLength] = 0;
        responseLength = recordLength + 3;
    } else {
        responsePdu = 0x01;
        parameters[0] = 0x03;
        parameters[1] = 0x00;
        responseLength = 2;
    }

    uint8_t response[520] = {};
    response[0] = responsePdu;
    writeBe16(response + 1, transaction);
    writeBe16(response + 3, (uint16_t)responseLength);
    memcpy(response + 5, parameters, responseLength);
    (void)send(fd, response, responseLength + 5, MSG_NOSIGNAL);
}

void BluetoothContext::handleAvdtpPacket(const uint8_t *packet, size_t length) {
    if (!packet || length < 2) {
        return;
    }
    const uint8_t transaction = packet[0] >> 4;
    const uint8_t packetType = (packet[0] >> 2) & 0x03;
    const uint8_t messageType = packet[0] & 0x03;
    const uint8_t signal = packet[1] & 0x3F;
    if (packetType != 0 || messageType != 0) {
        return;
    }

    uint8_t response[64] = {};
    size_t responseLength = 2;
    response[0] = (uint8_t)((transaction << 4) | 0x02);
    response[1] = signal;
    const uint8_t *payload = packet + 2;
    size_t payloadLength = length - 2;
    bool accept = true;

    switch (signal) {
    case 0x01: // Discover
        response[2] = 0x04; // SEID 1, Audio Sink, not in use.
        response[3] = 0x08; // Audio media type, Sink TSEP.
        responseLength = 4;
        break;
    case 0x02: // GetCapabilities
    case 0x0C: // GetAllCapabilities
        if (payloadLength < 1 || (payload[0] >> 2) != 1) {
            accept = false;
            break;
        }
        response[2] = 0x01;
        response[3] = 0x00;
        response[4] = 0x07;
        response[5] = 0x06;
        response[6] = 0x00;
        response[7] = 0x00;
        response[8] = 0x11; // 48 kHz, joint stereo.
        response[9] = 0xFF; // All SBC block lengths, subbands, and allocations.
        response[10] = 0x02;
        response[11] = 0x35;
        responseLength = 12;
        break;
    case 0x03: { // SetConfiguration
        if (payloadLength < 2 || (payload[0] >> 2) != 1) {
            accept = false;
            break;
        }
        uint8_t candidate[4] = {};
        bool gotSbc = false;
        for (size_t offset = 2; offset + 2 <= payloadLength;) {
            const uint8_t category = payload[offset++];
            const uint8_t categoryLength = payload[offset++];
            if (offset + categoryLength > payloadLength) {
                break;
            }
            if (category == 0x07 && categoryLength == 6 && payload[offset + 1] == 0x00) {
                memcpy(candidate, payload + offset + 2, sizeof(candidate));
                gotSbc = true;
            }
            offset += categoryLength;
        }
        if (!gotSbc || (candidate[0] & 0x10U) == 0 ||
            (candidate[0] & 0x0FU) == 0 || (candidate[1] & 0x15U) != 0x15U ||
            candidate[2] > 2 || candidate[3] < 2 || candidate[2] > candidate[3] ||
            !configureSbcDecoder(candidate)) {
            accept = false;
            break;
        }
        memcpy(sbcConfiguration, candidate, sizeof(sbcConfiguration));
        avdtpConfigured = true;
        break;
    }
    case 0x06: // Open
        if (payloadLength < 1 || !avdtpConfigured || (payload[0] >> 2) != 1) {
            accept = false;
            break;
        }
        avdtpOpened = true;
        break;
    case 0x07: // Start
        if (payloadLength < 1 || !avdtpOpened || (payload[0] >> 2) != 1) {
            accept = false;
            break;
        }
        avdtpStreaming = true;
        printf("[BT] [INFO] A2DP stream started\n");
        break;
    case 0x09: // Suspend
        avdtpStreaming = false;
        break;
    case 0x08: // Close
        avdtpStreaming = false;
        avdtpOpened = false;
        avdtpConfigured = false;
        if (mediaFd >= 0) {
            close(mediaFd);
            mediaFd = -1;
        }
        if (sbcDecoderInitialized) {
            sbc_finish(&sbcDecoder);
            sbcDecoderInitialized = false;
        }
        break;
    default:
        accept = false;
        break;
    }

    if (!accept) {
        response[0] = (uint8_t)((transaction << 4) | 0x03);
        response[2] = 0x31;
        responseLength = 3;
    }
    (void)send(signalingFd, response, responseLength, MSG_NOSIGNAL);
}

void BluetoothContext::handleA2dpMedia(const uint8_t *packet, size_t length) {
    if (!packet || length < 13 || (packet[0] >> 6) != 2) {
        return;
    }
    size_t headerLength = 12U + (size_t)(packet[0] & 0x0FU) * 4U;
    if (headerLength > length) {
        return;
    }
    if (packet[0] & 0x10U) {
        if (headerLength + 4 > length) {
            return;
        }
        uint16_t extensionWords = readBe16(packet + headerLength + 2);
        headerLength += 4U + (size_t)extensionWords * 4U;
        if (headerLength > length) {
            return;
        }
    }
    size_t payloadLength = length - headerLength;
    if (packet[0] & 0x20U) {
        uint8_t padding = packet[length - 1];
        if (padding == 0 || padding > payloadLength) {
            return;
        }
        payloadLength -= padding;
    }
    if (payloadLength > 0) {
        (void)decodeSbcPayload(packet + headerLength, payloadLength);
    }
}

void BluetoothContext::resetA2dpStream() {
    avdtpConfigured = false;
    avdtpOpened = false;
    avdtpStreaming = false;
    sbcFragmentSize = 0;
    sbcFragmentActive = false;
    if (sbcDecoderInitialized) {
        sbc_finish(&sbcDecoder);
        sbcDecoderInitialized = false;
    }
}

bool BluetoothContext::beginOutputConnect(const bt::Address &target) {
    if (outputSignalingFd >= 0 || outputConnected.load(std::memory_order_acquire)) {
        return false;
    }
    if (app && app->audio) {
        app->audio->setBluetoothOutputActive(false);
        outputDroppedAtStats = app->audio->bluetoothOutputDropped.load(std::memory_order_acquire);
    }
    outputTransaction = 0;
    outputPendingSignal = 0;
    outputSeid = 0;
    outputConfigured = false;
    outputOpened = false;
    outputMediaConnecting = false;
    outputPacketLength = 0;
    rtpSequence = 0;
    rtpTimestamp = 0;

    outputSignalingFd = socket(BT_AF_BLUETOOTH,
                               SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
                               BT_PROTO_L2CAP);
    if (outputSignalingFd < 0) {
        printf("[BT] [WARN] AVDTP signaling socket failed: %s\n", strerror(errno));
        return false;
    }
    Security security = {BT_SECURITY_MEDIUM, 0};
    if (setsockopt(outputSignalingFd, BT_SOL_BLUETOOTH, BT_SECURITY_OPT,
                   &security, sizeof(security)) < 0) {
        printf("[BT] [WARN] could not require security for A2DP output: %s\n", strerror(errno));
    }
    L2capAddress remote = {};
    remote.family = BT_AF_BLUETOOTH;
    remote.psm = htole16(BT_PSM_AVDTP);
    memcpy(remote.bdaddr, target.b, sizeof(remote.bdaddr));
    if (connect(outputSignalingFd, (struct sockaddr *)&remote, sizeof(remote)) == 0) {
        outputConnecting = false;
        sendOutputCommand(0x01, nullptr, 0);
    } else if (errno == EINPROGRESS) {
        outputConnecting = true;
    } else {
        printf("[BT] [WARN] AVDTP signaling connect failed: %s\n", strerror(errno));
        close(outputSignalingFd);
        outputSignalingFd = -1;
        return false;
    }
    printf("[BT] [INFO] connecting A2DP source to hci%d peer\n", devId);
    return true;
}

void BluetoothContext::sendOutputCommand(uint8_t signal, const uint8_t *payload, size_t length) {
    if (outputSignalingFd < 0 || length > 1000) {
        disconnectOutput("invalid AVDTP command");
        return;
    }
    uint8_t packet[1024] = {};
    const uint8_t transaction = outputTransaction++ & 0x0F;
    packet[0] = (uint8_t)(transaction << 4);
    packet[1] = signal;
    if (payload && length > 0) {
        memcpy(packet + 2, payload, length);
    }
    ssize_t sent = send(outputSignalingFd, packet, length + 2, MSG_NOSIGNAL);
    if (sent != (ssize_t)(length + 2)) {
        if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            outputPendingSignal = signal;
            return;
        }
        disconnectOutput(sent < 0 ? strerror(errno) : "short AVDTP command write");
        return;
    }
    outputPendingSignal = signal;
}

void BluetoothContext::handleOutputSignaling() {
    uint8_t packet[1024] = {};
    ssize_t length = read(outputSignalingFd, packet, sizeof(packet));
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    if (length < 2) {
        disconnectOutput(length < 0 ? strerror(errno) : "AVDTP peer disconnected");
        return;
    }
    const uint8_t transaction = packet[0] >> 4;
    const uint8_t packetType = (packet[0] >> 2) & 0x03;
    const uint8_t messageType = packet[0] & 0x03;
    const uint8_t signal = packet[1] & 0x3F;
    if (packetType != 0 || transaction != ((outputTransaction - 1U) & 0x0FU) ||
        signal != outputPendingSignal) {
        return;
    }
    if (messageType != 2) {
        uint8_t error = length >= 4 ? packet[3] : 0;
        char message[BT_MESSAGE_MAX];
        snprintf(message, sizeof(message), "AVDTP signal 0x%02x rejected (0x%02x)", signal, error);
        disconnectOutput(message);
        return;
    }
    outputPendingSignal = 0;
    advanceOutputNegotiation(signal, packet + 2, (size_t)length - 2U);
}

void BluetoothContext::advanceOutputNegotiation(uint8_t signal,
                                                const uint8_t *payload,
                                                size_t length) {
    if (signal == 0x01) {
        bool foundSink = false;
        for (size_t offset = 0; offset + 2 <= length; offset += 2) {
            uint8_t mediaType = (payload[offset + 1] >> 4) & 0x0F;
            bool isSink = (payload[offset + 1] & 0x08U) != 0;
            if (mediaType == 0 && isSink) {
                outputSeid = payload[offset] >> 2;
                foundSink = true;
                break;
            }
        }
        if (!foundSink) {
            disconnectOutput("peer has no A2DP Sink endpoint");
            return;
        }
        uint8_t seid = (uint8_t)(outputSeid << 2);
        sendOutputCommand(0x02, &seid, 1);
        return;
    }

    if (signal == 0x02 || signal == 0x0C) {
        const uint8_t *remoteSbc = nullptr;
        for (size_t offset = 0; offset + 2 <= length;) {
            const uint8_t category = payload[offset++];
            const uint8_t categoryLength = payload[offset++];
            if (offset + categoryLength > length) {
                break;
            }
            if (category == 0x07 && categoryLength == 6 &&
                payload[offset] == 0 && payload[offset + 1] == 0) {
                remoteSbc = payload + offset + 2;
            }
            offset += categoryLength;
        }
        if (!remoteSbc || (remoteSbc[0] & 0x10U) == 0) {
            disconnectOutput("peer SBC capabilities do not include 48 kHz");
            return;
        }
        uint8_t channelMode = 0;
        if (remoteSbc[0] & 0x01U) channelMode = 0x01; // joint stereo
        else if (remoteSbc[0] & 0x02U) channelMode = 0x02; // stereo
        else if (remoteSbc[0] & 0x04U) channelMode = 0x04; // dual channel
        if (channelMode == 0) {
            disconnectOutput("peer has no stereo SBC mode");
            return;
        }
        uint8_t selected[4] = {};
        selected[0] = (uint8_t)(0x10U | channelMode);
        if (remoteSbc[1] & 0x10U) selected[1] |= 0x10U;
        else if (remoteSbc[1] & 0x20U) selected[1] |= 0x20U;
        else if (remoteSbc[1] & 0x40U) selected[1] |= 0x40U;
        else if (remoteSbc[1] & 0x80U) selected[1] |= 0x80U;
        if (remoteSbc[1] & 0x04U) selected[1] |= 0x04U;
        else if (remoteSbc[1] & 0x08U) selected[1] |= 0x08U;
        if (remoteSbc[1] & 0x01U) selected[1] |= 0x01U;
        else if (remoteSbc[1] & 0x02U) selected[1] |= 0x02U;
        if ((selected[1] & 0xFCU) == 0 || (selected[1] & 0x03U) == 0) {
            disconnectOutput("peer has no compatible SBC framing");
            return;
        }
        selected[2] = remoteSbc[2] > 2 ? remoteSbc[2] : 2;
        selected[3] = remoteSbc[3] < BT_A2DP_TARGET_BITPOOL
            ? remoteSbc[3] : BT_A2DP_TARGET_BITPOOL;
        if (selected[3] < selected[2]) {
            disconnectOutput("peer SBC bitpool range is invalid");
            return;
        }
        if (sbcEncoderInitialized) {
            sbc_finish(&sbcEncoder);
            sbcEncoderInitialized = false;
        }
        if (sbc_init_a2dp(&sbcEncoder, 0, selected, sizeof(selected)) < 0) {
            disconnectOutput("SBC encoder rejected peer configuration");
            return;
        }
        sbcEncoder.bitpool = selected[3];
        sbcEncoderInitialized = true;
         printf("[BT] [INFO] SBC output config: frequency=%u mode=%u blocks=%u subbands=%u bitpool=%u frame_length=%zu code_size=%zu\n",
             sbcEncoder.frequency == SBC_FREQ_48000 ? 48000U :
             sbcEncoder.frequency == SBC_FREQ_44100 ? 44100U :
             sbcEncoder.frequency == SBC_FREQ_32000 ? 32000U : 16000U,
             (unsigned)sbcEncoder.mode,
             sbcEncoder.blocks == SBC_BLK_16 ? 16U :
             sbcEncoder.blocks == SBC_BLK_12 ? 12U :
             sbcEncoder.blocks == SBC_BLK_8 ? 8U : 4U,
             sbcEncoder.subbands == SBC_SB_8 ? 8U : 4U,
             (unsigned)sbcEncoder.bitpool,
             sbc_get_frame_length(&sbcEncoder),
             sbc_get_codesize(&sbcEncoder));
        uint8_t config[10] = {
            (uint8_t)(outputSeid << 2),
            (uint8_t)(2U << 2),
            0x01, 0x00,
            0x07, 0x06, 0x00, 0x00,
            selected[0], selected[1],
        };
        uint8_t fullConfig[12] = {};
        memcpy(fullConfig, config, sizeof(config));
        fullConfig[10] = selected[2];
        fullConfig[11] = selected[3];
        outputConfigured = true;
        sendOutputCommand(0x03, fullConfig, sizeof(fullConfig));
        return;
    }

    if (signal == 0x03) {
        if (!outputConfigured) {
            disconnectOutput("A2DP configuration was not initialized");
            return;
        }
        outputOpened = false;
        uint8_t seid = (uint8_t)(outputSeid << 2);
        sendOutputCommand(0x06, &seid, 1);
        return;
    }

    if (signal == 0x06) {
        outputOpened = true;
        if (!connectOutputMedia()) {
            disconnectOutput("could not open A2DP media channel");
        }
        return;
    }

    if (signal == 0x07) {
        outputConnecting = false;
        outputConnectingStatus.store(0, std::memory_order_release);
        outputConnected.store(1, std::memory_order_release);
        if (app && app->audio) {
            app->audio->setBluetoothOutputActive(true);
        }
        setMessage("Connected to A2DP output");
        printf("[BT] [INFO] A2DP output stream started\n");
    }
}

bool BluetoothContext::connectOutputMedia() {
    outputMediaFd = socket(BT_AF_BLUETOOTH,
                           SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC,
                           BT_PROTO_L2CAP);
    if (outputMediaFd < 0) {
        return false;
    }
    Security security = {BT_SECURITY_MEDIUM, 0};
    (void)setsockopt(outputMediaFd, BT_SOL_BLUETOOTH, BT_SECURITY_OPT,
                     &security, sizeof(security));
    L2capOptions l2capOptions = {};
    l2capOptions.omtu = BT_A2DP_REQUESTED_MEDIA_MTU;
    l2capOptions.imtu = BT_A2DP_REQUESTED_MEDIA_MTU;
    l2capOptions.flushTo = 0xFFFF;
    if (setsockopt(outputMediaFd, BT_SOL_L2CAP, BT_L2CAP_OPTIONS,
                   &l2capOptions, sizeof(l2capOptions)) < 0) {
        printf("[BT] [WARN] could not request larger A2DP media MTU: %s\n", strerror(errno));
    }
    L2capAddress remote = {};
    remote.family = BT_AF_BLUETOOTH;
    remote.psm = htole16(BT_PSM_AVDTP);
    memcpy(remote.bdaddr, outputTarget.b, sizeof(remote.bdaddr));
    if (connect(outputMediaFd, (struct sockaddr *)&remote, sizeof(remote)) == 0) {
        if (!readOutputMediaMtu()) {
            close(outputMediaFd);
            outputMediaFd = -1;
            return false;
        }
        const uint8_t seid = (uint8_t)(outputSeid << 2);
        sendOutputCommand(0x07, &seid, 1);
        return true;
    }
    if (errno == EINPROGRESS) {
        outputMediaConnecting = true;
        return true;
    }
    close(outputMediaFd);
    outputMediaFd = -1;
    return false;
}

bool BluetoothContext::readOutputMediaMtu() {
    if (outputMediaFd < 0) {
        return false;
    }
    L2capOptions options = {};
    socklen_t length = sizeof(options);
    if (getsockopt(outputMediaFd, BT_SOL_L2CAP, BT_L2CAP_OPTIONS,
                   &options, &length) < 0 || length < sizeof(options)) {
        printf("[BT] [WARN] L2CAP_OPTIONS read failed: %s\n", strerror(errno));
        return false;
    }
    uint16_t mtu = options.omtu;
    if (mtu < BT_A2DP_DEFAULT_MEDIA_MTU) {
        mtu = BT_A2DP_DEFAULT_MEDIA_MTU;
    }
    if (mtu > BT_A2DP_MAX_MEDIA_MTU) {
        mtu = BT_A2DP_MAX_MEDIA_MTU;
    }
    outputMediaMtu = mtu;
    printf("[BT] [INFO] A2DP media MTU: outgoing=%u incoming=%u\n",
           (unsigned)options.omtu,
           (unsigned)options.imtu);
    return true;
}

void BluetoothContext::handleOutputMediaWritable() {
    if (outputMediaFd < 0 || outputPacketLength == 0) {
        return;
    }
    ssize_t sent = send(outputMediaFd, outputPacket, outputPacketLength, MSG_NOSIGNAL);
    if (sent == (ssize_t)outputPacketLength) {
        outputPacketLength = 0;
        ++outputPacketsSinceStats;
        return;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        ++outputSendWouldBlock;
        return;
    }
    disconnectOutput(sent < 0 ? strerror(errno) : "short A2DP media write");
}

void BluetoothContext::pumpOutputAudio() {
    if (!outputConnected.load(std::memory_order_acquire) || !app || !app->audio) {
        return;
    }
    const uint64_t nowMs = bt::monotonicMs();
    if (nowMs >= outputStatsNextMs) {
        const uint32_t queuedFrames = app->audio->getBluetoothOutputAvailable();
        const float ratio = app->audio->bluetoothOutputRateRatio;
        const uint32_t dropped = app->audio->bluetoothOutputDropped.load(std::memory_order_relaxed);
        const uint32_t droppedThisSecond = dropped - outputDroppedAtStats;
                const uint32_t wouldBlockThisSecond = outputSendWouldBlock - outputWouldBlockAtStats;
                 printf("[BT] [INFO] A2DP output: queued=%u frames rate_adjust=%+.0f ppm mtu=%u frames/packet=%u sent=%u/s send_eagain=%u dropped=%u bitpool=%u frame_length=%zu\n",
               queuedFrames,
               (double)((ratio - 1.0f) * 1000000.0f),
                             (unsigned)outputMediaMtu,
             (unsigned)outputFramesPerPacket,
               outputPacketsSinceStats,
                             wouldBlockThisSecond,
             droppedThisSecond,
             sbcEncoderInitialized ? (unsigned)sbcEncoder.bitpool : 0U,
             sbcEncoderInitialized ? sbc_get_frame_length(&sbcEncoder) : 0U);
        outputPacketsSinceStats = 0;
        outputDroppedAtStats = dropped;
                outputWouldBlockAtStats = outputSendWouldBlock;
        outputStatsNextMs = nowMs + 1000ULL;
        if (droppedThisSecond > SAMPLE_RATE / 20U) {
            disconnectOutput("PCM output queue is overrunning; stopping glitchy stream");
            return;
        }
    }
    if (outputPacketLength != 0 || !sbcEncoderInitialized || outputMediaFd < 0) {
        return;
    }
    const size_t codeSize = sbc_get_codesize(&sbcEncoder);
    if (codeSize == 0 || codeSize > 512 || (codeSize % 4) != 0) {
        disconnectOutput("unsupported negotiated SBC frame size");
        return;
    }
    const uint32_t samplesPerFrame = (uint32_t)(codeSize / 4);
    const size_t sbcFrameLength = sbc_get_frame_length(&sbcEncoder);
    if (sbcFrameLength == 0 || sbcFrameLength >= outputMediaMtu - BT_RTP_SBC_HEADER_SIZE) {
        disconnectOutput("SBC frame does not fit media MTU");
        return;
    }
    uint32_t frameCount = (outputMediaMtu - BT_RTP_SBC_HEADER_SIZE) / (uint32_t)sbcFrameLength;
    if (frameCount > BT_MAX_SBC_FRAMES_PER_PACKET) {
        frameCount = BT_MAX_SBC_FRAMES_PER_PACKET;
    }
    if (frameCount < 3U) {
        disconnectOutput("media MTU permits fewer than three SBC frames per packet");
        return;
    }
    outputFramesPerPacket = (uint8_t)frameCount;
    const uint32_t pcmFrames = samplesPerFrame * frameCount;
    if (app->audio->getBluetoothOutputAvailable() < pcmFrames) {
        return;
    }
    int16_t pcm[BT_MAX_SBC_FRAMES_PER_PACKET * 128U * 2U];
    if (pcmFrames > BT_MAX_SBC_FRAMES_PER_PACKET * 128U ||
        app->audio->resampleBluetoothOutputPcm(pcm, pcmFrames) != pcmFrames) {
        return;
    }

    uint8_t packet[BT_A2DP_MAX_MEDIA_MTU] = {};
    packet[0] = 0x80;
    packet[1] = 96;
    writeBe16(packet + 2, (uint16_t)rtpSequence++);
    packet[4] = (uint8_t)(rtpTimestamp >> 24);
    packet[5] = (uint8_t)(rtpTimestamp >> 16);
    packet[6] = (uint8_t)(rtpTimestamp >> 8);
    packet[7] = (uint8_t)rtpTimestamp;
    packet[8] = 'A'; packet[9] = 'u'; packet[10] = 'd'; packet[11] = 'X';
    packet[12] = (uint8_t)frameCount;
    size_t packetLength = BT_RTP_SBC_HEADER_SIZE;
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        ssize_t written = 0;
        ssize_t consumed = sbc_encode(&sbcEncoder,
                                      pcm + frame * (uint32_t)(codeSize / sizeof(int16_t)),
                                      codeSize,
                                      packet + packetLength,
                                      sizeof(packet) - packetLength,
                                      &written);
        if (consumed != (ssize_t)codeSize || written <= 0 ||
            (size_t)written > sizeof(packet) - packetLength) {
            disconnectOutput("SBC encoding failed");
            return;
        }
        packetLength += (size_t)written;
    }
    rtpTimestamp += pcmFrames;
    memcpy(outputPacket, packet, packetLength);
    outputPacketLength = packetLength;
}

void BluetoothContext::disconnectOutput(const char *reason) {
    if (app && app->audio) {
        app->audio->setBluetoothOutputActive(false);
    }
    int *fds[] = {&outputSignalingFd, &outputMediaFd};
    for (int *fd : fds) {
        if (*fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
    if (sbcEncoderInitialized) {
        sbc_finish(&sbcEncoder);
        sbcEncoderInitialized = false;
    }
    outputConnecting = false;
    outputConnectingStatus.store(0, std::memory_order_release);
    outputMediaConnecting = false;
    outputConfigured = false;
    outputOpened = false;
    outputConnected.store(0, std::memory_order_release);
    outputPacketLength = 0;
    outputPacketsSinceStats = 0;
    outputStatsNextMs = 0;
    if (reason && reason[0]) {
        printf("[BT] [INFO] A2DP output disconnected: %s\n", reason);
        setMessage("A2DP output: %s", reason);
    }
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

void BluetoothContext::runUnpair(const bt::Address &target) {
    if (outputConnected.load(std::memory_order_acquire) && bt::sameAddress(outputTarget, target)) {
        disconnectOutput("device forgotten");
    }
    if (outputConnectingStatus.load(std::memory_order_acquire) && bt::sameAddress(outputTarget, target)) {
        disconnectOutput("connection cancelled for forget");
    }

    uint8_t params[7] = {};
    memcpy(params, target.b, sizeof(target.b));
    params[6] = 0;
    if (agentFd >= 0 &&
        (bt::writeCommand(agentFd, BT_CMD_DELETE_STORED_LINK_KEY, params, sizeof(params)) != RET_OK ||
         bt::waitForCommand(agentFd, BT_CMD_DELETE_STORED_LINK_KEY, 1000) != 0)) {
        printf("[BT] [WARN] controller did not confirm link key deletion\n");
    }

    char address[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(target, address, sizeof(address));
    {
        std::lock_guard<std::mutex> lock(mutex);
        BluetoothDevice *device = findDevice(target);
        if (device) {
            device->paired = false;
            memset(device->linkKey, 0, sizeof(device->linkKey));
            device->linkKeyType = 0;
        }
    }
    savePaired();
    if (app && app->audio) {
        int graphRc = app->audio->reloadRoutingGraph();
        if (graphRc == RET_ERR) {
            printf("[BT] [WARN] graph refresh after forget failed\n");
        }
    }
    printf("[BT] [INFO] forgot paired device %s\n", address);
    setMessage("Forgot %s", address);
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
        if (app && app->audio) {
            int graphRc = app->audio->reloadRoutingGraph();
            if (graphRc == RET_ERR) {
                printf("[BT] [WARN] paired output graph refresh failed\n");
            }
        }
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
