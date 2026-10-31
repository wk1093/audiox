#include "bluetooth/context.hpp"
#include "json.hpp"
#include "bluetooth/a2dp.hpp"
#include "bluetooth/route_id.hpp"

#include "audio/context.hpp"
#include "audio/health.hpp"
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
#define BT_OUTPUT_CONNECT_TIMEOUT_MS 30000ULL
#define BT_OUTPUT_REFUSALS_BEFORE_INPUT 2U
// How long an AudioX-opened input channel waits for the source to configure the
// stream before AudioX configures it.
#define BT_INPUT_ACCEPT_WAIT_MS 3000ULL
#define BT_USER_DISCONNECT_REASON "disconnected by user"
#define BT_OUTPUT_SILENCE_THRESHOLD 2500ULL
#define BT_AUTOCONNECT_RETRY_MIN_MS 5000ULL
#define BT_AUTOCONNECT_RETRY_MAX_MS 60000ULL
#define BT_ADAPTER_RETRY_MS 10000ULL
#define BT_AUTOCONNECT_CHECK_MS 500ULL
// Minimum gap between auto-connect pages while any Bluetooth stream is running.
#define BT_AUTOCONNECT_STREAMING_GAP_MS 30000ULL
#define BT_AVDTP_RESPONSE_TIMEOUT_MS 10000ULL
#define BT_INPUT_STATS_INTERVAL_MS 5000ULL

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
#define BT_A2DP_FLUSH_TIMEOUT_MS 20U
#define BT_A2DP_MEDIA_SNDBUF 4096U
#define BT_A2DP_TARGET_BITPOOL 20U
#define BT_RTP_SBC_HEADER_SIZE 13U
#define BT_MAX_SBC_FRAMES_PER_PACKET 8U

#define BT_EVT_AUTH_COMPLETE 0x06
#define BT_EVT_CONN_REQUEST 0x04
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
#define BT_CMD_WRITE_SCAN_ENABLE 0x0C1A
#define BT_CMD_WRITE_CLASS_OF_DEVICE 0x0C24
#define BT_CMD_PIN_CODE_NEG_REPLY 0x040E
#define BT_CMD_IO_CAPABILITY_NEG_REPLY 0x0434
#define BT_SCAN_PAGE 0x02
// Audio + Rendering service, Audio/Video major class, Loudspeaker minor class.
#define BT_CLASS_OF_DEVICE 0x240414U
#define BT_PAIRING_NOT_ALLOWED 0x18

#define AVDTP_DISCOVER 0x01
#define AVDTP_GET_CAPABILITIES 0x02
#define AVDTP_SET_CONFIGURATION 0x03
#define AVDTP_GET_CONFIGURATION 0x04
#define AVDTP_RECONFIGURE 0x05
#define AVDTP_OPEN 0x06
#define AVDTP_START 0x07
#define AVDTP_CLOSE 0x08
#define AVDTP_SUSPEND 0x09
#define AVDTP_ABORT 0x0A
#define AVDTP_GET_ALL_CAPABILITIES 0x0C
#define AVDTP_DELAY_REPORT 0x0D
#define AVDTP_BAD_ACP_SEID 0x12
#define AVDTP_SEP_IN_USE 0x13
#define AVDTP_BAD_PAYLOAD_FORMAT 0x18
#define AVDTP_NOT_SUPPORTED_COMMAND 0x19
#define AVDTP_UNSUPPORTED_CONFIGURATION 0x29
#define AVDTP_BAD_STATE 0x31
#define AVDTP_SINK_SEID 1U
#define AVDTP_SOURCE_SEID 2U

#define BT_IO_CAP_NO_INPUT_NO_OUTPUT 0x03
#define BT_AUTH_GENERAL_BONDING 0x04
#define BT_COD_MAJOR_PHONE 0x02
#define BT_LINK_TYPE_ACL 0x01

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
      sdpClientFds{-1, -1, -1, -1},
      adapterReady(false),
      scanning(false),
      pairing(false),
      request(BT_REQUEST_NONE),
      pairTarget(),
      connectQueued(false),
      message(),
      devices(),
      deviceCount(0),
      links(),
      linkStatus(),
      realtimeScheduling(false),
      autoConnectNextDevice(0),
      lastAutoConnectMs(0),
      autoConnectCheckMs(0),
      adapterRetryMs(0),
      inboundPhone(),
      pairKeyStored(false),
      pairFailure(0) {
    for (BluetoothLink &link : links) {
        link.signalingFd = -1;
        link.inMediaFd = -1;
        link.outMediaFd = -1;
    }
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
    closeAllLinks(nullptr);
    if (agentFd >= 0) {
        close(agentFd);
        agentFd = -1;
    }
    closeA2dpSockets();
}

namespace {

bool anyLinkUsed(const BluetoothLinkStatus *status) {
    for (size_t i = 0; i < BT_MAX_LINKS; ++i) {
        if (status[i].used) {
            return true;
        }
    }
    return false;
}

} // namespace

int BluetoothContext::requestScan() {
    std::lock_guard<std::mutex> lock(mutex);
    // Scanning and pairing stall the worker thread, which also carries audio.
    if (request != BT_REQUEST_NONE || scanning || pairing || connectQueued || anyLinkUsed(linkStatus)) {
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
    // Scanning and pairing stall the worker thread, which also carries audio.
    if (request != BT_REQUEST_NONE || scanning || pairing || connectQueued || anyLinkUsed(linkStatus)) {
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
    if (request != BT_REQUEST_NONE || scanning || pairing || connectQueued) {
        return RET_WARN;
    }
    size_t freeLinks = 0;
    for (const BluetoothLinkStatus &status : linkStatus) {
        if (!status.used) {
            ++freeLinks;
            continue;
        }
        // One connection attempt at a time, and never two links to one device.
        if (status.connecting || bt::sameAddress(status.address, target)) {
            return RET_WARN;
        }
    }
    if (freeLinks == 0) {
        snprintf(message, sizeof(message), "Disconnect a Bluetooth device first (%u at most)",
                 (unsigned)BT_MAX_LINKS);
        return RET_WARN;
    }
    device->autoConnectHeld = false;
    device->autoConnectBackoffMs = BT_AUTOCONNECT_RETRY_MIN_MS;
    device->autoConnectAtMs = 0;
    pairTarget = target;
    request = BT_REQUEST_CONNECT;
    connectQueued = true;
    snprintf(message, sizeof(message), "Connecting to %s...", device->name[0] ? device->name : address);
    return RET_OK;
}

int BluetoothContext::requestDisconnect(const char *address) {
    bt::Address target = {};
    if (address && address[0] &&
        (!bt::parseAddress(address, &target) || bt::isZeroAddress(target))) {
        return RET_ERR;
    }
    std::lock_guard<std::mutex> lock(mutex);
    if (request != BT_REQUEST_NONE || scanning || pairing) {
        return RET_WARN;
    }
    const bool all = bt::isZeroAddress(target);
    bool matches = false;
    for (const BluetoothLinkStatus &status : linkStatus) {
        if (status.used && (all || bt::sameAddress(status.address, target))) {
            matches = true;
        }
    }
    if (!matches) {
        return RET_WARN;
    }
    pairTarget = target;
    request = BT_REQUEST_DISCONNECT;
    snprintf(message, sizeof(message), "Disconnecting Bluetooth audio...");
    return RET_OK;
}

int BluetoothContext::requestAutoConnect(const char *address, bool enabled) {
    bt::Address target = {};
    if (!bt::parseAddress(address, &target) || bt::isZeroAddress(target)) {
        return RET_ERR;
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        BluetoothDevice *device = findDevice(target);
        if (!device || !device->paired || !device->classic) {
            return RET_ERR;
        }
        device->autoConnect = enabled;
        if (enabled) {
            device->autoConnectHeld = false;
            device->autoConnectAtMs = 0;
            device->autoConnectBackoffMs = BT_AUTOCONNECT_RETRY_MIN_MS;
        }
    }
    return savePaired() ? RET_OK : RET_WARN;
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
    if (request != BT_REQUEST_NONE || scanning || pairing || connectQueued) {
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
    bool connecting = connectQueued;
    bool outputUp = false;
    bool outputPlaying = false;
    bool inputUp = false;
    bool inputPlaying = false;
    for (const BluetoothLinkStatus &status : linkStatus) {
        if (!status.used) {
            continue;
        }
        connecting = connecting || status.connecting;
        outputUp = outputUp || status.outputConnected;
        outputPlaying = outputPlaying || status.outputStreaming;
        inputUp = inputUp || status.inputConnected;
        inputPlaying = inputPlaying || status.inputStreaming;
    }
    auto flag = [](bool value) { return value ? "true" : "false"; };
    int n = snprintf(out, outSize,
                     "{\"ok\":true,\"ready\":%s,\"scanning\":%s,\"pairing\":%s,\"connecting\":%s,\"connected\":%s,\"streaming\":%s,"
                     "\"output_connected\":%s,\"output_streaming\":%s,\"input_connected\":%s,\"input_streaming\":%s,"
                     "\"max_connections\":%u,\"message\":\"%s\",\"devices\":[",
                     flag(adapterReady),
                     flag(scanning),
                     flag(pairing),
                     flag(connecting),
                     flag(outputUp || inputUp),
                     flag(outputPlaying || inputPlaying),
                     flag(outputUp),
                     flag(outputPlaying),
                     flag(inputUp),
                     flag(inputPlaying),
                     (unsigned)BT_MAX_LINKS,
                     audiox::json::escape(message).c_str());
    if (n < 0 || (size_t)n >= outSize) {
        return RET_ERR;
    }
    size_t used = (size_t)n;
    for (size_t i = 0; i < deviceCount; ++i) {
        const BluetoothDevice &device = devices[i];
        char address[BT_ADDRESS_TEXT] = {};
        bt::formatAddress(device.address, address, sizeof(address));
        BluetoothLinkStatus here = {};
        for (const BluetoothLinkStatus &status : linkStatus) {
            if (status.used && bt::sameAddress(status.address, device.address)) {
                here = status;
                break;
            }
        }
        const bool deviceConnecting = here.connecting ||
            (connectQueued && request == BT_REQUEST_CONNECT && bt::sameAddress(pairTarget, device.address));
        n = snprintf(out + used, outSize - used,
                     "%s{\"address\":\"%s\",\"name\":\"%s\",\"paired\":%s,\"classic\":%s,\"connecting\":%s,\"connected\":%s,\"streaming\":%s,"
                     "\"output_connected\":%s,\"output_streaming\":%s,\"input_connected\":%s,\"input_streaming\":%s,"
                     "\"auto_connect\":%s,\"audio_source\":%s,\"audio_sink\":%s}",
                     i > 0 ? "," : "",
                     address,
                     audiox::json::escape(device.name).c_str(),
                     flag(device.paired),
                     flag(device.classic),
                     flag(deviceConnecting),
                     flag(here.outputConnected || here.inputConnected),
                     flag(here.outputStreaming || here.inputStreaming),
                     flag(here.outputConnected),
                     flag(here.outputStreaming),
                     flag(here.inputConnected),
                     flag(here.inputStreaming),
                     flag(device.paired && device.autoConnect),
                     flag(device.audioSource),
                     flag(device.audioSink));
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

bool BluetoothContext::getOutputDeviceName(char *out, size_t outSize, bool *connected) const {
    if (connected) {
        *connected = false;
    }
    if (!out || outSize == 0) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex);
    const BluetoothDevice *selected = nullptr;
    for (const BluetoothLinkStatus &status : linkStatus) {
        if (status.used && status.outputConnected) {
            selected = findDevice(status.address);
            if (selected) {
                if (connected) {
                    *connected = true;
                }
                break;
            }
        }
    }
    for (size_t i = 0; !selected && i < deviceCount; ++i) {
        if (devices[i].paired && devices[i].classic &&
            !(devices[i].audioSource && !devices[i].audioSink)) {
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

const BluetoothDevice *BluetoothContext::findDevice(const bt::Address &address) const {
    for (size_t i = 0; i < deviceCount; ++i) {
        if (bt::sameAddress(devices[i].address, address)) {
            return &devices[i];
        }
    }
    return nullptr;
}

// Device name, or its address when unnamed. Must not hold mutex.
void BluetoothContext::deviceLabel(const bt::Address &address, char *out, size_t outSize) {
    {
        std::lock_guard<std::mutex> lock(mutex);
        const BluetoothDevice *device = findDevice(address);
        if (device && device->name[0]) {
            snprintf(out, outSize, "%s", device->name);
            return;
        }
    }
    bt::formatAddress(address, out, outSize);
}


namespace {

int socketError(int fd) {
    int error = 0;
    socklen_t errorLength = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &errorLength) < 0) {
        error = errno;
    }
    return error;
}

void closeFd(int &fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

void resetLink(BluetoothLink &link) {
    memset(&link, 0, sizeof(link));
    link.signalingFd = -1;
    link.inMediaFd = -1;
    link.outMediaFd = -1;
    link.outSuspendSupported = true;
    link.mediaMtu = BT_A2DP_DEFAULT_MEDIA_MTU;
}

} // namespace

int BluetoothContext::findLink(const bt::Address &address) const {
    for (int i = 0; i < BT_MAX_LINKS; ++i) {
        if (links[i].used && bt::sameAddress(links[i].address, address)) {
            return i;
        }
    }
    return -1;
}

int BluetoothContext::allocateLink(const bt::Address &address) {
    for (int i = 0; i < BT_MAX_LINKS; ++i) {
        if (!links[i].used) {
            resetLink(links[i]);
            links[i].used = true;
            links[i].address = address;
            return i;
        }
    }
    return -1;
}

bool BluetoothContext::linkIdle(int index) const {
    const BluetoothLink &link = links[index];
    return !link.wantOutput && !link.outUp && !link.wantInput && !link.inConfigured;
}

bool BluetoothContext::configureSbcDecoder(int index, const uint8_t configuration[4]) {
    BluetoothLink &link = links[index];
    if (!configuration) {
        return false;
    }
    if (link.decoderReady) {
        sbc_finish(&link.decoder);
        link.decoderReady = false;
    }
    if (sbc_init_a2dp(&link.decoder, 0, configuration, 4) < 0) {
        printf("[BT] [WARN] SBC decoder rejected negotiated configuration\n");
        return false;
    }
    link.decoderReady = true;
    link.decoderChannels = link.decoder.mode == SBC_MODE_MONO ? 1 : 2;
    link.fragmentSize = 0;
    link.fragmentActive = false;
    printf("[BT] [INFO] SBC decoder ready: %u Hz, %u channel(s), bitpool %u\n",
           bt::sbcSampleRate(configuration),
           (unsigned)link.decoderChannels,
           (unsigned)link.decoder.bitpool);
    return true;
}

uint32_t BluetoothContext::decodeSbcPayload(int index, const uint8_t *payload, size_t length) {
    BluetoothLink &link = links[index];
    if (!link.decoderReady || !app || !app->audio || !payload || length < 2) {
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
            link.fragmentSize = 0;
            link.fragmentActive = true;
        } else if (!link.fragmentActive) {
            return 0;
        }
        if (fragmentLength > sizeof(link.fragment) - link.fragmentSize) {
            link.fragmentActive = false;
            link.fragmentSize = 0;
            return 0;
        }
        memcpy(link.fragment + link.fragmentSize, fragment, fragmentLength);
        link.fragmentSize += fragmentLength;
        if (!last) {
            return 0;
        }
        uint8_t completeFrame[sizeof(link.fragment) + 1];
        completeFrame[0] = 1;
        memcpy(completeFrame + 1, link.fragment, link.fragmentSize);
        const size_t completeLength = link.fragmentSize + 1;
        link.fragmentActive = false;
        link.fragmentSize = 0;
        return decodeSbcPayload(index, completeFrame, completeLength);
    }
    if (start || last) {
        return 0;
    }
    const uint8_t frameCount = packetHeader & 0x0FU;
    if (frameCount == 0) {
        return 0;
    }

    BluetoothInputStream &stream = app->audio->bluetoothInputs[index];
    const uint8_t *frame = payload + 1;
    size_t remaining = length - 1;
    uint32_t totalFrames = 0;
    int16_t decoded[16U * 8U * 2U];
    int16_t stereo[16U * 8U * 2U];
    for (uint8_t i = 0; i < frameCount && remaining > 0; ++i) {
        size_t decodedBytes = 0;
        const ssize_t consumed = sbc_decode(&link.decoder, frame, remaining,
                                            decoded, sizeof(decoded), &decodedBytes);
        if (consumed <= 0 || (size_t)consumed > remaining ||
            decodedBytes == 0 || decodedBytes > sizeof(decoded)) {
            ++link.inDecodeErrors;
            break;
        }
        const uint32_t channels = link.decoderChannels;
        const uint32_t samples = (uint32_t)(decodedBytes / (sizeof(int16_t) * channels));
        if (samples > 16U * 8U) {
            break;
        }
        for (uint32_t sample = 0; sample < samples; ++sample) {
            stereo[sample * 2U] = decoded[sample * channels];
            stereo[sample * 2U + 1U] = channels == 2 ? decoded[sample * 2U + 1U] : decoded[sample];
        }
        totalFrames += stream.push(stereo, samples);
        frame += (size_t)consumed;
        remaining -= (size_t)consumed;
    }
    return totalFrames;
}

void BluetoothContext::threadMain() {
    printf("[BT] [INFO] bluetooth thread started\n");
    bool firstAttempt = true;
    while (run.load(std::memory_order_acquire)) {
        bool wantRealtime = false;
        bool anyOutputUp = false;
        for (const BluetoothLink &link : links) {
            if (link.used && (link.outUp || link.inStreaming)) {
                wantRealtime = true;
            }
            if (link.used && link.outUp) {
                anyOutputUp = true;
            }
        }
        if (wantRealtime != realtimeScheduling) {
            sched_param priority = {};
            const int policy = wantRealtime ? SCHED_FIFO : SCHED_OTHER;
            priority.sched_priority = wantRealtime ? 98 : 0;
            const int rc = pthread_setschedparam(pthread_self(), policy, &priority);
            if (rc != 0) {
                printf("[BT] [WARN] cannot set Bluetooth worker scheduler policy=%d priority=%d: %s\n",
                       policy, priority.sched_priority, strerror(rc));
            }
            realtimeScheduling = wantRealtime;
        }

        uint8_t next = BT_REQUEST_NONE;
        bt::Address target = {};
        {
            std::lock_guard<std::mutex> lock(mutex);
            next = request;
            target = pairTarget;
            request = BT_REQUEST_NONE;
        }

        const uint64_t loopNowMs = bt::monotonicMs();
        if (!adapterReady &&
            (firstAttempt || next != BT_REQUEST_NONE || loopNowMs >= adapterRetryMs)) {
            firstAttempt = false;
            if (!bringUpAdapter()) {
                std::lock_guard<std::mutex> lock(mutex);
                scanning = false;
                pairing = false;
                snprintf(message, sizeof(message), "Bluetooth adapter unavailable");
                adapterRetryMs = loopNowMs + BT_ADAPTER_RETRY_MS;
                next = BT_REQUEST_NONE;
            } else {
                adapterRetryMs = 0;
            }
        }

        if (next == BT_REQUEST_SCAN) {
            runScan();
            publishLinkStatus();
            continue;
        }
        if (next == BT_REQUEST_PAIR) {
            runPair(target);
            publishLinkStatus();
            continue;
        }
        if (next == BT_REQUEST_CONNECT && !beginConnect(target, false)) {
            scheduleAutoConnectRetry(target);
        }
        if (next == BT_REQUEST_DISCONNECT) {
            const bool all = bt::isZeroAddress(target);
            for (int i = 0; i < BT_MAX_LINKS; ++i) {
                if (links[i].used && (all || bt::sameAddress(links[i].address, target))) {
                    holdAutoConnect(links[i].address);
                    closeLink(i, BT_USER_DISCONNECT_REASON);
                }
            }
        }
        if (next == BT_REQUEST_UNPAIR) {
            runUnpair(target);
        }

        const uint64_t nowMs = bt::monotonicMs();
        for (int i = 0; i < BT_MAX_LINKS; ++i) {
            serviceLinkTimers(i, nowMs);
        }
        if (next == BT_REQUEST_NONE) {
            runAutoConnect(nowMs);
        }
        for (int i = 0; i < BT_MAX_LINKS; ++i) {
            pumpOutputAudio(i);
        }
        publishLinkStatus();

        enum {
            POLL_AGENT,
            POLL_SDP_LISTEN,
            POLL_AVDTP_LISTEN,
            POLL_SDP_CLIENTS,
        };
        constexpr size_t sdpClientCount = sizeof(sdpClientFds) / sizeof(sdpClientFds[0]);
        constexpr size_t linkBase = POLL_SDP_CLIENTS + sdpClientCount;
        struct pollfd pollFds[linkBase + BT_MAX_LINKS * 3U] = {};
        pollFds[POLL_AGENT] = {agentFd, POLLIN, 0};
        pollFds[POLL_SDP_LISTEN] = {sdpListenFd, POLLIN, 0};
        pollFds[POLL_AVDTP_LISTEN] = {avdtpListenFd, POLLIN, 0};
        for (size_t i = 0; i < sdpClientCount; ++i) {
            pollFds[POLL_SDP_CLIENTS + i] = {sdpClientFds[i], POLLIN, 0};
        }
        for (int i = 0; i < BT_MAX_LINKS; ++i) {
            const BluetoothLink &link = links[i];
            struct pollfd *slot = &pollFds[linkBase + (size_t)i * 3U];
            slot[0] = {-1, 0, 0};
            slot[1] = {-1, 0, 0};
            slot[2] = {-1, 0, 0};
            if (!link.used) {
                continue;
            }
            slot[0] = {link.signalingFd,
                       (short)(link.signalingConnecting ? POLLOUT
                                                        : (POLLIN | (link.commandWritePending ? POLLOUT : 0))),
                       0};
            slot[1] = {link.inMediaFd, (short)(link.inMediaConnecting ? POLLOUT : POLLIN), 0};
            slot[2] = {link.outMediaFd,
                       (short)((link.outMediaConnecting || link.packetLength > 0) ? POLLOUT : 0), 0};
        }
        const int pollTimeout = anyOutputUp ? 2 : 200;
        if (poll(pollFds, sizeof(pollFds) / sizeof(pollFds[0]), pollTimeout) > 0) {
            if (agentFd >= 0 && (pollFds[POLL_AGENT].revents & POLLIN)) {
                uint8_t packet[300];
                const ssize_t length = read(agentFd, packet, sizeof(packet));
                if (length > 0) {
                    handleAgentEvent(packet, (size_t)length);
                }
            }
            if (sdpListenFd >= 0 && (pollFds[POLL_SDP_LISTEN].revents & POLLIN)) {
                acceptSdpRequest();
            }
            for (size_t i = 0; i < sdpClientCount; ++i) {
                const struct pollfd &client = pollFds[POLL_SDP_CLIENTS + i];
                if (client.fd >= 0 && client.fd == sdpClientFds[i] &&
                    (client.revents & (POLLIN | POLLERR | POLLHUP)) &&
                    !handleSdpRequest(sdpClientFds[i])) {
                    close(sdpClientFds[i]);
                    sdpClientFds[i] = -1;
                }
            }
            for (int i = 0; i < BT_MAX_LINKS; ++i) {
                const struct pollfd *slot = &pollFds[linkBase + (size_t)i * 3U];
                BluetoothLink &link = links[i];
                if (link.used && slot[0].fd >= 0 && slot[0].fd == link.signalingFd) {
                    if (link.signalingConnecting) {
                        if (slot[0].revents & (POLLOUT | POLLERR | POLLHUP)) {
                            handleSignalingConnected(i);
                        }
                    } else {
                        if (slot[0].revents & (POLLIN | POLLERR | POLLHUP)) {
                            handleSignalingReadable(i);
                        }
                        if (link.used && slot[0].fd == link.signalingFd && link.commandWritePending &&
                            (slot[0].revents & POLLOUT)) {
                            flushCommand(i);
                        }
                    }
                }
                if (link.used && slot[1].fd >= 0 && slot[1].fd == link.inMediaFd) {
                    if (link.inMediaConnecting) {
                        if (slot[1].revents & (POLLOUT | POLLERR | POLLHUP)) {
                            const int error = socketError(link.inMediaFd);
                            if (error != 0) {
                                closeInputStream(i, strerror(error));
                                if (linkIdle(i)) {
                                    closeLink(i, "could not open the A2DP input media channel");
                                }
                            } else {
                                handleMediaConnected(i, true);
                            }
                        }
                    } else if (slot[1].revents & (POLLIN | POLLERR | POLLHUP)) {
                        readInputMedia(i);
                    }
                }
                if (link.used && slot[2].fd >= 0 && slot[2].fd == link.outMediaFd) {
                    if (link.outMediaConnecting) {
                        if (slot[2].revents & (POLLOUT | POLLERR | POLLHUP)) {
                            const int error = socketError(link.outMediaFd);
                            if (error != 0) {
                                closeOutputStream(i, strerror(error));
                            } else {
                                handleMediaConnected(i, false);
                            }
                        }
                    } else if (link.packetLength > 0 && (slot[2].revents & POLLOUT)) {
                        flushOutputPacket(i);
                    } else if (slot[2].revents & (POLLERR | POLLHUP)) {
                        closeOutputStream(i, "A2DP media channel closed");
                    }
                }
            }
            if (avdtpListenFd >= 0 && (pollFds[POLL_AVDTP_LISTEN].revents & POLLIN)) {
                acceptA2dpChannel();
            }
        }
        for (int i = 0; i < BT_MAX_LINKS; ++i) {
            pumpOutputAudio(i);
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

    const uint8_t classOfDevice[] = {
        (uint8_t)BT_CLASS_OF_DEVICE,
        (uint8_t)(BT_CLASS_OF_DEVICE >> 8),
        (uint8_t)(BT_CLASS_OF_DEVICE >> 16),
    };
    if (bt::writeCommand(fd, BT_CMD_WRITE_CLASS_OF_DEVICE, classOfDevice, sizeof(classOfDevice)) != RET_OK ||
        bt::waitForCommand(fd, BT_CMD_WRITE_CLASS_OF_DEVICE, 1000) != 0) {
        printf("[BT] [WARN] could not set Bluetooth device class to audio speaker\n");
    }

    // Page scan lets paired phones connect to AudioX. Inquiry scan stays off,
    // so AudioX is not discoverable; new devices are paired from the web UI.
    const uint8_t scanEnable[] = {BT_SCAN_PAGE};
    if (bt::writeCommand(fd, BT_CMD_WRITE_SCAN_ENABLE, scanEnable, sizeof(scanEnable)) != RET_OK ||
        bt::waitForCommand(fd, BT_CMD_WRITE_SCAN_ENABLE, 1000) != 0) {
        printf("[BT] [WARN] could not enable page scan; devices cannot connect to AudioX\n");
    } else {
        printf("[BT] [INFO] connectable by paired devices\n");
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
    closeAllLinks(nullptr);
    int *fds[] = {&sdpListenFd, &avdtpListenFd};
    for (int *fd : fds) {
        if (*fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
    for (int &fd : sdpClientFds) {
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
}

void BluetoothContext::acceptSdpRequest() {
    int fd = accept4(sdpListenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        return;
    }
    // Requests arrive after the channel opens and may repeat, so the channel
    // stays open until the client hangs up.
    for (int &slot : sdpClientFds) {
        if (slot < 0) {
            slot = fd;
            return;
        }
    }
    close(sdpClientFds[0]);
    for (size_t i = 1; i < sizeof(sdpClientFds) / sizeof(sdpClientFds[0]); ++i) {
        sdpClientFds[i - 1] = sdpClientFds[i];
    }
    sdpClientFds[sizeof(sdpClientFds) / sizeof(sdpClientFds[0]) - 1] = fd;
}

bool BluetoothContext::handleSdpRequest(int fd) {
    uint8_t request[1024] = {};
    ssize_t length = read(fd, request, sizeof(request));
    if (length < 0) {
        return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    }
    if (length == 0) {
        return false;
    }
    uint8_t response[512] = {};
    const size_t responseLength = bt::buildSdpResponse(request, (size_t)length, response, sizeof(response));
    if (responseLength > 0) {
        (void)send(fd, response, responseLength, MSG_NOSIGNAL);
    }
    return true;
}


void BluetoothContext::acceptA2dpChannel() {
    int fd = accept4(avdtpListenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0) {
        return;
    }
    L2capAddress peer = {};
    socklen_t peerLength = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &peerLength) != 0) {
        close(fd);
        return;
    }
    bt::Address address = {};
    memcpy(address.b, peer.bdaddr, sizeof(address.b));
    char addressText[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(address, addressText, sizeof(addressText));

    const int existing = findLink(address);
    if (existing >= 0) {
        BluetoothLink &link = links[existing];
        if (link.signalingConnecting) {
            // The device called AudioX while AudioX was still calling it; keep
            // the device's channel and drop ours.
            closeFd(link.signalingFd);
            link.signalingFd = fd;
            link.signalingConnecting = false;
            printf("[BT] [INFO] AVDTP signaling channel connected from %s (crossed connect)\n", addressText);
            handleSignalingConnected(existing);
            return;
        }
        // Second channel from a connected device: the media transport of a
        // stream the device opened on AudioX's sink.
        if (link.inOpened && link.inMediaFd < 0 && !link.inMediaConnecting) {
            link.inMediaFd = fd;
            printf("[BT] [INFO] A2DP input media channel connected from %s\n", addressText);
            return;
        }
        printf("[BT] [INFO] ignored extra AVDTP channel from %s\n", addressText);
        close(fd);
        return;
    }

    const int index = allocateLink(address);
    if (index < 0) {
        printf("[BT] [WARN] %s connected but %u Bluetooth devices are already connected\n",
               addressText, (unsigned)BT_MAX_LINKS);
        setMessage("%s: disconnect another Bluetooth device first", addressText);
        close(fd);
        return;
    }
    bool knownSource = false;
    bool knownSink = false;
    char name[BT_NAME_MAX] = {};
    {
        std::lock_guard<std::mutex> lock(mutex);
        BluetoothDevice *device = findDevice(address);
        if (device) {
            knownSource = device->audioSource;
            knownSink = device->audioSink;
            device->autoConnectHeld = false;
            device->autoConnectAtMs = 0;
            device->autoConnectBackoffMs = BT_AUTOCONNECT_RETRY_MIN_MS;
            snprintf(name, sizeof(name), "%s", device->name);
        }
    }
    const uint64_t nowMs = bt::monotonicMs();
    BluetoothLink &link = links[index];
    link.signalingFd = fd;
    if (knownSink && !knownSource) {
        // A speaker reconnecting to AudioX expects AudioX to start the stream.
        link.wantOutput = true;
        link.discoverAtMs = 0;
    } else {
        // A source normally configures its stream right away. If it does not
        // (or the device turns out to be a speaker), AudioX takes over.
        link.wantInput = true;
        link.wantOutput = knownSink;
        link.discoverAtMs = knownSink ? 0 : nowMs + BT_INPUT_ACCEPT_WAIT_MS;
    }
    link.inInitiateAtMs = nowMs + BT_INPUT_ACCEPT_WAIT_MS;
    link.connecting = link.wantOutput;
    link.deadlineMs = link.wantOutput ? nowMs + BT_OUTPUT_CONNECT_TIMEOUT_MS : 0;
    printf("[BT] [INFO] AVDTP signaling channel connected from %s\n", addressText);
    if (link.wantOutput) {
        setMessage("%s connected; starting audio output...", name[0] ? name : addressText);
    } else {
        setMessage("%s connected as an audio input", name[0] ? name : addressText);
    }
    driveLink(index);
}

bool BluetoothContext::beginConnect(const bt::Address &target, bool automatic) {
    char targetText[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(target, targetText, sizeof(targetText));
    if (findLink(target) >= 0) {
        return true;
    }
    bool source = false;
    bool sink = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        const BluetoothDevice *device = findDevice(target);
        if (!device || !device->paired) {
            return false;
        }
        source = device->audioSource;
        sink = device->audioSink;
    }
    const int index = allocateLink(target);
    if (index < 0) {
        setMessage("Disconnect a Bluetooth device first (%u at most)", (unsigned)BT_MAX_LINKS);
        return false;
    }
    int fd = socket(BT_AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BT_PROTO_L2CAP);
    if (fd < 0) {
        printf("[BT] [WARN] AVDTP signaling socket failed: %s\n", strerror(errno));
        resetLink(links[index]);
        return false;
    }
    Security security = {BT_SECURITY_MEDIUM, 0};
    if (setsockopt(fd, BT_SOL_BLUETOOTH, BT_SECURITY_OPT, &security, sizeof(security)) < 0) {
        printf("[BT] [WARN] could not require security for A2DP: %s\n", strerror(errno));
    }
    L2capAddress remote = {};
    remote.family = BT_AF_BLUETOOTH;
    remote.psm = htole16(BT_PSM_AVDTP);
    memcpy(remote.bdaddr, target.b, sizeof(remote.bdaddr));
    bool inProgress = false;
    if (connect(fd, (struct sockaddr *)&remote, sizeof(remote)) != 0) {
        if (errno != EINPROGRESS) {
            printf("[BT] [WARN] AVDTP signaling connect failed: %s\n", strerror(errno));
            close(fd);
            resetLink(links[index]);
            return false;
        }
        inProgress = true;
    }
    BluetoothLink &link = links[index];
    const bool inputOnly = source && !sink;
    link.signalingFd = fd;
    link.signalingConnecting = inProgress;
    link.connecting = true;
    link.deadlineMs = bt::monotonicMs() + BT_OUTPUT_CONNECT_TIMEOUT_MS;
    link.wantOutput = !inputOnly;
    link.wantInput = source;
    link.discoverAtMs = 0;
    printf("[BT] [INFO] %sconnecting A2DP %s %s on hci%d\n",
           automatic ? "auto-" : "",
           inputOnly ? "input from" : "to",
           targetText, devId);
    if (!inProgress) {
        handleSignalingConnected(index);
    }
    return true;
}

void BluetoothContext::handleSignalingConnected(int index) {
    BluetoothLink &link = links[index];
    const bt::Address address = link.address;
    const bool wantOutput = link.wantOutput;
    if (link.signalingConnecting) {
        const int error = socketError(link.signalingFd);
        if (error != 0) {
            const bool refused = error == ECONNREFUSED;
            closeLink(index, refused
                ? "device refused the connection; if it is a phone, connect from the phone, or forget AudioX on the device and pair again from AudioX"
                : strerror(error));
            if (wantOutput) {
                noteOutputChannelResult(address, refused);
            }
            return;
        }
        link.signalingConnecting = false;
    }
    if (wantOutput) {
        noteOutputChannelResult(address, false);
    }
    const uint64_t nowMs = bt::monotonicMs();
    if (link.inInitiateAtMs == 0 || link.inInitiateAtMs < nowMs) {
        link.inInitiateAtMs = nowMs + BT_INPUT_ACCEPT_WAIT_MS;
    }
    char addressText[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(address, addressText, sizeof(addressText));
    printf("[BT] [INFO] AVDTP signaling channel connected to %s\n", addressText);
    if (!wantOutput && link.connecting) {
        link.connecting = false;
        link.deadlineMs = 0;
        resetAutoConnectBackoff(address);
        char label[BT_NAME_MAX] = {};
        deviceLabel(address, label, sizeof(label));
        setMessage("%s connected as an audio input", label);
    }
    driveLink(index);
}

bool BluetoothContext::sendCommand(int index, uint8_t signal, const uint8_t *payload, size_t length, bool input) {
    BluetoothLink &link = links[index];
    if (!link.used || link.signalingFd < 0 || link.signalingConnecting ||
        length > sizeof(link.commandPacket) - 2U || link.pendingSignal != 0 || link.commandWritePending) {
        return false;
    }
    const uint8_t transaction = link.transaction++ & 0x0F;
    link.commandPacket[0] = (uint8_t)(transaction << 4);
    link.commandPacket[1] = signal;
    if (payload && length > 0) {
        memcpy(link.commandPacket + 2, payload, length);
    }
    link.commandLength = length + 2U;
    link.pendingSignal = signal;
    link.pendingTransaction = transaction;
    link.pendingInput = input;
    link.pendingSinceMs = bt::monotonicMs();
    const ssize_t sent = send(link.signalingFd, link.commandPacket, link.commandLength, MSG_NOSIGNAL);
    if (sent == (ssize_t)link.commandLength) {
        link.commandLength = 0;
        return true;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        link.commandWritePending = true;
        return true;
    }
    closeLink(index, sent < 0 ? strerror(errno) : "short AVDTP command write");
    return false;
}

void BluetoothContext::flushCommand(int index) {
    BluetoothLink &link = links[index];
    if (!link.commandWritePending || link.commandLength == 0) {
        link.commandWritePending = false;
        return;
    }
    const ssize_t sent = send(link.signalingFd, link.commandPacket, link.commandLength, MSG_NOSIGNAL);
    if (sent == (ssize_t)link.commandLength) {
        link.commandLength = 0;
        link.commandWritePending = false;
        return;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    closeLink(index, sent < 0 ? strerror(errno) : "short AVDTP command write");
}

// Advances whatever AudioX still has to set up on this link, one command at a time.
void BluetoothContext::driveLink(int index) {
    BluetoothLink &link = links[index];
    if (!link.used || link.signalingFd < 0 || link.signalingConnecting ||
        link.pendingSignal != 0 || link.commandWritePending) {
        return;
    }
    const uint64_t nowMs = bt::monotonicMs();
    if (!link.discovered) {
        // A source that configured its own stream needs nothing from AudioX.
        if (!link.wantOutput && (link.inRemoteActive || link.inConfigured)) {
            return;
        }
        if (nowMs < link.discoverAtMs) {
            return;
        }
        sendCommand(index, AVDTP_DISCOVER, nullptr, 0, false);
        return;
    }
    if (link.wantOutput && !link.outUp) {
        if (!link.outStarting && link.remoteSinkSeid != 0) {
            link.outStarting = true;
            const uint8_t seid = (uint8_t)(link.remoteSinkSeid << 2);
            sendCommand(index, AVDTP_GET_CAPABILITIES, &seid, 1, false);
        }
        // The input waits until the output is running.
        return;
    }
    if (link.wantInput && link.remoteSourceSeid != 0 && !link.inConfigured && !link.inRemoteActive &&
        nowMs >= link.inInitiateAtMs) {
        const uint8_t seid = (uint8_t)(link.remoteSourceSeid << 2);
        sendCommand(index, AVDTP_GET_CAPABILITIES, &seid, 1, true);
    }
}

void BluetoothContext::serviceLinkTimers(int index, uint64_t nowMs) {
    BluetoothLink &link = links[index];
    if (!link.used) {
        return;
    }
    if (link.connecting && link.deadlineMs != 0 && nowMs >= link.deadlineMs) {
        closeLink(index, "A2DP connection timed out");
        return;
    }
    if (link.pendingSignal != 0 && !link.signalingConnecting &&
        nowMs - link.pendingSinceMs >= BT_AVDTP_RESPONSE_TIMEOUT_MS) {
        closeLink(index, "device stopped answering A2DP signaling");
        return;
    }
    driveLink(index);
    if (link.used && link.inStreaming) {
        logInputStats(index, nowMs, false);
    }
}

void BluetoothContext::handleSignalingReadable(int index) {
    BluetoothLink &link = links[index];
    uint8_t packet[1024] = {};
    const ssize_t length = read(link.signalingFd, packet, sizeof(packet));
    if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
        return;
    }
    if (length <= 0) {
        // The device hanging up is deliberate; do not dial an input straight back.
        const bool deliberate = length == 0 || errno == ECONNRESET;
        closeLink(index, length == 0 ? "device disconnected" : strerror(errno), deliberate);
        return;
    }
    if (length < 2 || ((packet[0] >> 2) & 0x03) != 0) {
        return;
    }
    const uint8_t transaction = packet[0] >> 4;
    const uint8_t messageType = packet[0] & 0x03;
    const uint8_t signal = packet[1] & 0x3F;
    if (messageType == 0) {
        handleAvdtpCommand(index, packet, (size_t)length);
        return;
    }
    if (link.pendingSignal != 0 && signal == link.pendingSignal && transaction == link.pendingTransaction) {
        handleResponse(index, messageType, signal, packet + 2, (size_t)length - 2U);
    }
}

// Answers AVDTP commands for this device. SEID 1 is AudioX's A2DP sink (the
// input); SEID 2 is its A2DP source, only reported once the output configured it.
void BluetoothContext::handleAvdtpCommand(int index, const uint8_t *packet, size_t length) {
    BluetoothLink &link = links[index];
    if (link.signalingFd < 0 || !packet || length < 2) {
        return;
    }
    const int fd = link.signalingFd;
    const uint8_t transaction = packet[0] >> 4;
    const uint8_t signal = packet[1] & 0x3F;
    const uint8_t *payload = packet + 2;
    const size_t payloadLength = length - 2;
    const uint8_t acpSeid = payloadLength >= 1 ? (uint8_t)(payload[0] >> 2) : 0;
    uint8_t rejectSeid = acpSeid;
    const bool sinkSeid = acpSeid == AVDTP_SINK_SEID;
    const bool sourceSeid = acpSeid == AVDTP_SOURCE_SEID && link.outConfigured;
    BluetoothOutputStream *outputSlot = app && app->audio ? &app->audio->bluetoothOutputs[index] : nullptr;
    BluetoothInputStream *inputSlot = app && app->audio ? &app->audio->bluetoothInputs[index] : nullptr;

    uint8_t response[64] = {};
    size_t responseLength = 2;
    response[0] = (uint8_t)((transaction << 4) | 0x02);
    response[1] = signal;
    uint8_t error = 0;
    uint8_t errorCategory = 0;
    const char *closeInputReason = nullptr;
    const char *closeOutputReason = nullptr;
    bool inputConfigured = false;

    switch (signal) {
    case AVDTP_DISCOVER:
        response[responseLength++] = (uint8_t)((AVDTP_SINK_SEID << 2) | (link.inConfigured ? 0x02 : 0x00));
        response[responseLength++] = 0x08; // Audio, Sink
        if (link.outConfigured) {
            response[responseLength++] = (uint8_t)((AVDTP_SOURCE_SEID << 2) | 0x02);
            response[responseLength++] = 0x00; // Audio, Source
        }
        break;
    case AVDTP_GET_CAPABILITIES:
    case AVDTP_GET_ALL_CAPABILITIES:
        if (payloadLength < 1) {
            error = AVDTP_BAD_PAYLOAD_FORMAT;
        } else if (!sinkSeid) {
            error = AVDTP_BAD_ACP_SEID;
        } else {
            const uint8_t capabilities[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00};
            memcpy(response + responseLength, capabilities, sizeof(capabilities));
            responseLength += sizeof(capabilities);
            memcpy(response + responseLength, bt::A2DP_SBC_SINK_CAPABILITIES, 4);
            responseLength += 4;
        }
        break;
    case AVDTP_SET_CONFIGURATION:
    case AVDTP_RECONFIGURE: {
        const bool reconfigure = signal == AVDTP_RECONFIGURE;
        if (payloadLength < (reconfigure ? 1U : 2U)) {
            error = AVDTP_BAD_PAYLOAD_FORMAT;
            break;
        }
        if (!sinkSeid) {
            error = AVDTP_BAD_ACP_SEID;
            break;
        }
        if (!reconfigure && link.inConfigured) {
            error = AVDTP_SEP_IN_USE;
            break;
        }
        if (reconfigure && (!link.inOpened || link.inStreaming)) {
            error = AVDTP_BAD_STATE;
            break;
        }
        const size_t capabilitiesOffset = reconfigure ? 1U : 2U;
        const uint8_t *sbc = bt::findSbcCodec(payload + capabilitiesOffset,
                                             payloadLength - capabilitiesOffset);
        errorCategory = 0x07;
        if (!sbc || !bt::isValidSbcSinkConfiguration(sbc) || !configureSbcDecoder(index, sbc)) {
            error = AVDTP_UNSUPPORTED_CONFIGURATION;
            break;
        }
        errorCategory = 0;
        memcpy(link.inConfig, sbc, sizeof(link.inConfig));
        link.inConfigured = true;
        link.inRemoteActive = true;
        link.wantInput = true;
        if (!link.wantOutput) {
            link.connecting = false;
            link.deadlineMs = 0;
        }
        inputConfigured = true;
        break;
    }
    case AVDTP_GET_CONFIGURATION:
        if (!sinkSeid) {
            error = AVDTP_BAD_ACP_SEID;
        } else if (!link.inConfigured) {
            error = AVDTP_BAD_STATE;
        } else {
            const uint8_t configuration[] = {0x01, 0x00, 0x07, 0x06, 0x00, 0x00};
            memcpy(response + responseLength, configuration, sizeof(configuration));
            responseLength += sizeof(configuration);
            memcpy(response + responseLength, link.inConfig, sizeof(link.inConfig));
            responseLength += sizeof(link.inConfig);
        }
        break;
    case AVDTP_OPEN:
        if (!sinkSeid) {
            error = AVDTP_BAD_ACP_SEID;
        } else if (!link.inConfigured || link.inOpened) {
            error = AVDTP_BAD_STATE;
        } else {
            link.inOpened = true;
        }
        break;
    case AVDTP_START:
    case AVDTP_SUSPEND: {
        // The command may list several SEIDs; check them all before acting.
        const bool starting = signal == AVDTP_START;
        bool startOutput = false;
        bool startInput = false;
        bool stopOutput = false;
        bool stopInput = false;
        if (payloadLength < 1) {
            error = AVDTP_BAD_PAYLOAD_FORMAT;
        }
        for (size_t i = 0; i < payloadLength && !error; ++i) {
            const uint8_t seid = (uint8_t)(payload[i] >> 2);
            if (seid == AVDTP_SOURCE_SEID && link.outConfigured) {
                if (!link.outOpened) {
                    error = AVDTP_BAD_STATE;
                } else if (starting) {
                    startOutput = true;
                } else {
                    stopOutput = true;
                }
            } else if (seid == AVDTP_SINK_SEID) {
                if (!link.inOpened) {
                    error = AVDTP_BAD_STATE;
                } else if (starting) {
                    startInput = true;
                } else {
                    stopInput = true;
                }
            } else {
                error = AVDTP_BAD_ACP_SEID;
            }
            if (error) {
                rejectSeid = seid;
            }
        }
        if (error) {
            break;
        }
        if (startOutput) {
            link.outStartRequested = false;
            link.outStreaming = true;
            link.silenceSinceMs = bt::monotonicMs();
            if (outputSlot && !outputSlot->active.load(std::memory_order_acquire)) {
                outputSlot->setActive(true);
            }
        }
        if (stopOutput) {
            link.outStreaming = false;
            link.outSuspendRequested = false;
            link.outStartRequested = false;
            link.silenceSinceMs = 0;
            if (outputSlot) {
                outputSlot->setActive(false);
            }
        }
        if (startInput && !link.inStreaming) {
            startInputAudio(index);
        }
        if (stopInput && link.inStreaming) {
            logInputStats(index, bt::monotonicMs(), true);
            link.inStreaming = false;
            if (inputSlot) {
                inputSlot->stop();
            }
            char addressText[BT_ADDRESS_TEXT] = {};
            bt::formatAddress(link.address, addressText, sizeof(addressText));
            printf("[BT] [INFO] A2DP input from %s paused by device\n", addressText);
        }
        break;
    }
    case AVDTP_CLOSE:
    case AVDTP_ABORT:
        if (sourceSeid) {
            closeOutputReason = signal == AVDTP_CLOSE ? "device closed the stream" : "device aborted the stream";
        } else if (sinkSeid) {
            if (signal == AVDTP_CLOSE && !link.inConfigured) {
                error = AVDTP_BAD_STATE;
                break;
            }
            closeInputReason = signal == AVDTP_CLOSE ? "device closed the stream" : "device aborted the stream";
        } else if (signal == AVDTP_CLOSE) {
            error = AVDTP_BAD_ACP_SEID;
        }
        break;
    case AVDTP_DELAY_REPORT:
        if (!sourceSeid && !sinkSeid) {
            error = AVDTP_BAD_ACP_SEID;
        }
        break;
    default:
        // General reject for signals this endpoint does not know.
        response[0] = (uint8_t)((transaction << 4) | 0x01);
        (void)send(fd, response, 2, MSG_NOSIGNAL);
        return;
    }

    if (error) {
        // Only START and SUSPEND rejects carry the SEID; configuration rejects
        // carry the failing service category.
        response[0] = (uint8_t)((transaction << 4) | 0x03);
        responseLength = 2;
        if (signal == AVDTP_SET_CONFIGURATION || signal == AVDTP_RECONFIGURE) {
            response[responseLength++] = errorCategory;
        } else if (signal == AVDTP_START || signal == AVDTP_SUSPEND) {
            response[responseLength++] = (uint8_t)(rejectSeid << 2);
        }
        response[responseLength++] = error;
    }
    (void)send(fd, response, responseLength, MSG_NOSIGNAL);
    if (inputConfigured) {
        setDeviceRoles(link.address, 1, -1, "configured an A2DP input stream");
    }
    if (closeInputReason) {
        closeInputStream(index, closeInputReason);
    }
    if (closeOutputReason) {
        closeOutputStream(index, closeOutputReason);
    }
}

void BluetoothContext::handleResponse(int index, uint8_t messageType, uint8_t signal,
                                      const uint8_t *payload, size_t length) {
    BluetoothLink &link = links[index];
    const bool input = link.pendingInput;
    link.pendingSignal = 0;
    link.commandLength = 0;
    link.commandWritePending = false;
    if (signal == AVDTP_DISCOVER) {
        if (messageType != 2) {
            closeLink(index, "device rejected A2DP discovery");
            return;
        }
        handleDiscoverResponse(index, payload, length);
    } else if (input) {
        handleInputResponse(index, messageType, signal, payload, length);
    } else {
        handleOutputResponse(index, messageType, signal, payload, length);
    }
    driveLink(index);
}

void BluetoothContext::handleDiscoverResponse(int index, const uint8_t *payload, size_t length) {
    BluetoothLink &link = links[index];
    uint8_t sinkSeid = 0;
    uint8_t sourceSeid = 0;
    bool sinkBusy = false;
    bool sourceBusy = false;
    for (size_t offset = 0; offset + 2 <= length; offset += 2) {
        if ((payload[offset + 1] >> 4) != 0) {
            continue;
        }
        const uint8_t seid = payload[offset] >> 2;
        const bool inUse = (payload[offset] & 0x02U) != 0;
        if (payload[offset + 1] & 0x08U) {
            if (sinkSeid == 0 || (sinkBusy && !inUse)) {
                sinkSeid = seid;
                sinkBusy = inUse;
            }
        } else if (sourceSeid == 0 || (sourceBusy && !inUse)) {
            sourceSeid = seid;
            sourceBusy = inUse;
        }
    }
    link.discovered = true;
    link.remoteSinkSeid = sinkSeid;
    link.remoteSourceSeid = sourceSeid;
    const bt::Address address = link.address;
    char label[BT_NAME_MAX] = {};
    deviceLabel(address, label, sizeof(label));
    if (sinkSeid != 0 || sourceSeid != 0) {
        setDeviceRoles(address, sourceSeid ? 1 : 0, sinkSeid ? 1 : 0, "reported its A2DP endpoints");
    }
    const bool wasConnecting = link.connecting;
    link.wantOutput = sinkSeid != 0;
    link.wantInput = sourceSeid != 0 || link.inConfigured;
    if (!link.wantOutput && !link.wantInput) {
        closeLink(index, "device has no A2DP audio endpoint");
        return;
    }
    if (link.wantOutput && !link.connecting && !link.outUp) {
        link.connecting = true;
        link.deadlineMs = bt::monotonicMs() + BT_OUTPUT_CONNECT_TIMEOUT_MS;
    }
    if (!link.wantOutput && wasConnecting) {
        link.connecting = false;
        link.deadlineMs = 0;
        resetAutoConnectBackoff(address);
        setMessage("%s connected as an audio input", label);
    }
}

void BluetoothContext::handleOutputResponse(int index, uint8_t messageType, uint8_t signal,
                                            const uint8_t *payload, size_t length) {
    BluetoothLink &link = links[index];
    BluetoothOutputStream *slot = app && app->audio ? &app->audio->bluetoothOutputs[index] : nullptr;
    if (messageType != 2) {
        const uint8_t error = length > 0 ? payload[length - 1] : 0;
        if (signal == AVDTP_SUSPEND) {
            link.outSuspendSupported = false;
            link.outSuspendRequested = false;
            link.silenceSinceMs = bt::monotonicMs();
            setMessage("Connected; this device does not support idle audio pause");
            printf("[BT] [INFO] A2DP peer rejected idle suspend (0x%02x)\n", error);
            return;
        }
        char reason[BT_MESSAGE_MAX];
        snprintf(reason, sizeof(reason), "AVDTP signal 0x%02x rejected (0x%02x)", signal, error);
        closeOutputStream(index, reason);
        return;
    }

    if (signal == AVDTP_GET_CAPABILITIES || signal == AVDTP_GET_ALL_CAPABILITIES) {
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
            closeOutputStream(index, "device SBC audio does not include 48 kHz");
            return;
        }
        uint8_t channelMode = 0;
        if (remoteSbc[0] & 0x01U) channelMode = 0x01; // joint stereo
        else if (remoteSbc[0] & 0x02U) channelMode = 0x02; // stereo
        else if (remoteSbc[0] & 0x04U) channelMode = 0x04; // dual channel
        if (channelMode == 0) {
            closeOutputStream(index, "device has no stereo SBC mode");
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
            closeOutputStream(index, "device has no compatible SBC framing");
            return;
        }
        selected[2] = remoteSbc[2] > 2 ? remoteSbc[2] : 2;
        selected[3] = remoteSbc[3] < BT_A2DP_TARGET_BITPOOL ? remoteSbc[3] : BT_A2DP_TARGET_BITPOOL;
        if (selected[3] < selected[2]) {
            closeOutputStream(index, "device SBC bitpool range is invalid");
            return;
        }
        if (link.encoderReady) {
            sbc_finish(&link.encoder);
            link.encoderReady = false;
        }
        if (sbc_init_a2dp(&link.encoder, 0, selected, sizeof(selected)) < 0) {
            closeOutputStream(index, "SBC encoder rejected device configuration");
            return;
        }
        link.encoderReady = true;
        printf("[BT] [INFO] SBC output config: frequency=%u mode=%u blocks=%u subbands=%u bitpool=%u frame_length=%zu code_size=%zu\n",
               bt::sbcSampleRate(selected),
               (unsigned)link.encoder.mode,
               link.encoder.blocks == SBC_BLK_16 ? 16U :
               link.encoder.blocks == SBC_BLK_12 ? 12U :
               link.encoder.blocks == SBC_BLK_8 ? 8U : 4U,
               link.encoder.subbands == SBC_SB_8 ? 8U : 4U,
               (unsigned)link.encoder.bitpool,
               sbc_get_frame_length(&link.encoder),
               sbc_get_codesize(&link.encoder));
        const uint8_t config[] = {
            (uint8_t)(link.remoteSinkSeid << 2),
            (uint8_t)(AVDTP_SOURCE_SEID << 2),
            0x01, 0x00,
            0x07, 0x06, 0x00, 0x00,
            selected[0], selected[1], selected[2], selected[3],
        };
        sendCommand(index, AVDTP_SET_CONFIGURATION, config, sizeof(config), false);
        return;
    }

    if (signal == AVDTP_SET_CONFIGURATION) {
        link.outConfigured = true;
        const uint8_t seid = (uint8_t)(link.remoteSinkSeid << 2);
        sendCommand(index, AVDTP_OPEN, &seid, 1, false);
        return;
    }

    if (signal == AVDTP_OPEN) {
        link.outOpened = true;
        if (!connectMedia(index, false)) {
            closeOutputStream(index, "could not open A2DP media channel");
        }
        return;
    }

    if (signal == AVDTP_START) {
        const uint64_t nowMs = bt::monotonicMs();
        link.outStreaming = true;
        link.outStartRequested = false;
        link.outSuspendRequested = false;
        link.silenceSinceMs = nowMs;
        if (link.outUp) {
            if (slot && !slot->active.load(std::memory_order_acquire)) {
                slot->setActive(true);
            }
            return;
        }
        link.outUp = true;
        link.connecting = false;
        link.deadlineMs = 0;
        link.statsNextMs = 0;
        if (slot) {
            slot->peer.store(bt::packAddress(link.address.b), std::memory_order_release);
            slot->setActive(true);
            link.droppedAtStats = slot->dropped.load(std::memory_order_acquire);
        }
        // Give a dual-role device a moment to start its own input stream.
        if (link.inInitiateAtMs < nowMs + 1000ULL) {
            link.inInitiateAtMs = nowMs + 1000ULL;
        }
        const bt::Address address = link.address;
        resetAutoConnectBackoff(address);
        setDeviceRoles(address, -1, 1, "accepted an A2DP output stream");
        char label[BT_NAME_MAX] = {};
        deviceLabel(address, label, sizeof(label));
        char addressText[BT_ADDRESS_TEXT] = {};
        bt::formatAddress(address, addressText, sizeof(addressText));
        printf("[BT] [INFO] A2DP output stream started to %s\n", addressText);
        setMessage("Connected to %s", label);
        return;
    }

    if (signal == AVDTP_SUSPEND) {
        link.outStreaming = false;
        link.outSuspendRequested = false;
        link.outSuspendSupported = true;
        link.outStartRequested = false;
        link.silenceSinceMs = 0;
        if (slot) {
            slot->setActive(false);
        }
    }
}

void BluetoothContext::handleInputResponse(int index, uint8_t messageType, uint8_t signal,
                                           const uint8_t *payload, size_t length) {
    BluetoothLink &link = links[index];
    if (messageType != 2) {
        // The device configured the stream itself while AudioX was doing so.
        if (link.inRemoteActive) {
            return;
        }
        char reason[BT_MESSAGE_MAX];
        snprintf(reason, sizeof(reason), "A2DP input signal 0x%02x rejected (0x%02x)",
                 signal, length > 0 ? payload[length - 1] : 0);
        closeInputStream(index, reason);
        if (linkIdle(index)) {
            closeLink(index, reason);
        }
        return;
    }

    if (signal == AVDTP_GET_CAPABILITIES || signal == AVDTP_GET_ALL_CAPABILITIES) {
        if (link.inRemoteActive || link.inConfigured) {
            return;
        }
        const uint8_t *remoteSbc = bt::findSbcCodec(payload, length);
        uint8_t selected[4] = {};
        const char *failure = nullptr;
        if (!remoteSbc || !bt::selectSbcSinkConfiguration(remoteSbc, selected)) {
            failure = "device SBC audio is not available at 44.1 or 48 kHz";
        } else if (!configureSbcDecoder(index, selected)) {
            failure = "SBC decoder rejected device configuration";
        }
        if (failure) {
            closeInputStream(index, failure);
            if (linkIdle(index)) {
                closeLink(index, failure);
            }
            return;
        }
        memcpy(link.inConfig, selected, sizeof(link.inConfig));
        const uint8_t config[] = {
            (uint8_t)(link.remoteSourceSeid << 2),
            (uint8_t)(AVDTP_SINK_SEID << 2),
            0x01, 0x00,
            0x07, 0x06, 0x00, 0x00,
            selected[0], selected[1], selected[2], selected[3],
        };
        sendCommand(index, AVDTP_SET_CONFIGURATION, config, sizeof(config), true);
        return;
    }

    if (signal == AVDTP_SET_CONFIGURATION) {
        if (link.inRemoteActive) {
            return;
        }
        link.inConfigured = true;
        setDeviceRoles(link.address, 1, -1, "configured an A2DP input stream");
        if (!link.used) {
            return;
        }
        const uint8_t seid = (uint8_t)(link.remoteSourceSeid << 2);
        sendCommand(index, AVDTP_OPEN, &seid, 1, true);
        return;
    }

    if (signal == AVDTP_OPEN) {
        link.inOpened = true;
        if (!connectMedia(index, true)) {
            closeInputStream(index, "could not open A2DP input media channel");
            if (linkIdle(index)) {
                closeLink(index, "could not open A2DP input media channel");
            }
        }
    }
}


bool BluetoothContext::connectMedia(int index, bool input) {
    BluetoothLink &link = links[index];
    int &mediaFd = input ? link.inMediaFd : link.outMediaFd;
    if (mediaFd >= 0) {
        return true;
    }
    int fd = socket(BT_AF_BLUETOOTH, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, BT_PROTO_L2CAP);
    if (fd < 0) {
        return false;
    }
    Security security = {BT_SECURITY_MEDIUM, 0};
    (void)setsockopt(fd, BT_SOL_BLUETOOTH, BT_SECURITY_OPT, &security, sizeof(security));
    if (!input) {
        int sendBufferSize = BT_A2DP_MEDIA_SNDBUF;
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sendBufferSize, sizeof(sendBufferSize)) < 0) {
            printf("[BT] [WARN] could not limit A2DP media send buffer: %s\n", strerror(errno));
        }
        L2capOptions l2capOptions = {};
        l2capOptions.omtu = BT_A2DP_REQUESTED_MEDIA_MTU;
        l2capOptions.imtu = BT_A2DP_REQUESTED_MEDIA_MTU;
        l2capOptions.flushTo = BT_A2DP_FLUSH_TIMEOUT_MS;
        if (setsockopt(fd, BT_SOL_L2CAP, BT_L2CAP_OPTIONS, &l2capOptions, sizeof(l2capOptions)) < 0) {
            printf("[BT] [WARN] could not request larger A2DP media MTU: %s\n", strerror(errno));
        }
    }
    L2capAddress remote = {};
    remote.family = BT_AF_BLUETOOTH;
    remote.psm = htole16(BT_PSM_AVDTP);
    memcpy(remote.bdaddr, link.address.b, sizeof(remote.bdaddr));
    if (connect(fd, (struct sockaddr *)&remote, sizeof(remote)) == 0) {
        mediaFd = fd;
        handleMediaConnected(index, input);
        return true;
    }
    if (errno != EINPROGRESS) {
        printf("[BT] [WARN] A2DP %s media connect failed: %s\n", input ? "input" : "output", strerror(errno));
        close(fd);
        return false;
    }
    mediaFd = fd;
    if (input) {
        link.inMediaConnecting = true;
    } else {
        link.outMediaConnecting = true;
    }
    return true;
}

void BluetoothContext::handleMediaConnected(int index, bool input) {
    BluetoothLink &link = links[index];
    char addressText[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(link.address, addressText, sizeof(addressText));
    if (input) {
        link.inMediaConnecting = false;
        printf("[BT] [INFO] A2DP input media channel connected to %s; waiting for the device to play\n",
               addressText);
        return;
    }
    link.outMediaConnecting = false;
    if (!readMediaMtu(index)) {
        closeOutputStream(index, "could not read A2DP media MTU");
        return;
    }
    const uint8_t seid = (uint8_t)(link.remoteSinkSeid << 2);
    sendCommand(index, AVDTP_START, &seid, 1, false);
}

bool BluetoothContext::readMediaMtu(int index) {
    BluetoothLink &link = links[index];
    if (link.outMediaFd < 0) {
        return false;
    }
    L2capOptions options = {};
    socklen_t length = sizeof(options);
    if (getsockopt(link.outMediaFd, BT_SOL_L2CAP, BT_L2CAP_OPTIONS, &options, &length) < 0 ||
        length < sizeof(options)) {
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
    link.mediaMtu = mtu;
    printf("[BT] [INFO] A2DP media MTU: outgoing=%u incoming=%u\n",
           (unsigned)options.omtu, (unsigned)options.imtu);
    return true;
}

void BluetoothContext::readInputMedia(int index) {
    BluetoothLink &link = links[index];
    uint8_t packet[2048];
    for (int count = 0; count < 16 && link.inMediaFd >= 0; ++count) {
        const ssize_t length = read(link.inMediaFd, packet, sizeof(packet));
        if (length < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) {
            return;
        }
        if (length <= 0) {
            const char *reason = length == 0 ? "media channel closed" : strerror(errno);
            closeInputStream(index, reason);
            if (linkIdle(index)) {
                closeLink(index, reason);
            }
            return;
        }
        if (!link.inOpened) {
            continue;
        }
        // Some sources stream without sending START first.
        if (!link.inStreaming) {
            startInputAudio(index);
        }
        handleA2dpMedia(index, packet, (size_t)length);
    }
}

void BluetoothContext::handleA2dpMedia(int index, const uint8_t *packet, size_t length) {
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
    if (payloadLength == 0) {
        return;
    }
    BluetoothLink &link = links[index];
    const uint64_t nowMs = bt::monotonicMs();
    if (link.inLastPacketMs != 0) {
        const uint32_t gapMs = (uint32_t)(nowMs - link.inLastPacketMs);
        if (gapMs > link.inMaxGapMs) {
            link.inMaxGapMs = gapMs;
        }
        if (gapMs >= 60U) {
            ++link.inLongGaps;
        }
    }
    link.inLastPacketMs = nowMs;
    ++link.inPackets;
    const uint16_t sequence = readBe16(packet + 2);
    const uint32_t timestamp = ((uint32_t)packet[4] << 24) | ((uint32_t)packet[5] << 16) |
                               ((uint32_t)packet[6] << 8) | (uint32_t)packet[7];
    if (link.inRtpValid) {
        const uint16_t lost = (uint16_t)(sequence - (uint16_t)(link.inRtpSequence + 1U));
        if (lost != 0 && lost < 0x8000U) {
            link.inLostPackets += lost;
        }
        // The sender skipping audio shows up as a timestamp jump without lost packets.
        const uint32_t jump = timestamp - link.inRtpNextTimestamp;
        if (jump != 0 && jump < 0x80000000U) {
            link.inSkippedFrames += jump;
        }
    }
    const uint32_t frames = decodeSbcPayload(index, packet + headerLength, payloadLength);
    link.inFrames += frames;
    link.inRtpValid = true;
    link.inRtpSequence = sequence;
    link.inRtpNextTimestamp = timestamp + frames;
}

void BluetoothContext::startInputAudio(int index) {
    BluetoothLink &link = links[index];
    link.inStreaming = true;
    const uint32_t rate = bt::sbcSampleRate(link.inConfig);
    if (app && app->audio) {
        BluetoothInputStream &stream = app->audio->bluetoothInputs[index];
        stream.start(bt::packAddress(link.address.b), rate);
        link.inUnderrunsLogged = stream.underruns.load(std::memory_order_relaxed);
        link.inOverflowsLogged = stream.overflows.load(std::memory_order_relaxed);
        link.inDroppedLogged = stream.dropped.load(std::memory_order_relaxed);
    }
    const uint64_t nowMs = bt::monotonicMs();
    link.inStatsNextMs = nowMs + BT_INPUT_STATS_INTERVAL_MS;
    link.inStatsStartMs = nowMs;
    link.inLastPacketMs = 0;
    link.inPackets = 0;
    link.inFrames = 0;
    link.inMaxGapMs = 0;
    link.inLongGaps = 0;
    link.inLostPackets = 0;
    link.inSkippedFrames = 0;
    link.inDecodeErrors = 0;
    link.inRtpValid = false;
    char addressText[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(link.address, addressText, sizeof(addressText));
    printf("[BT] [INFO] A2DP input stream started from %s at %u Hz\n", addressText, rate);
}

// Report actual input trouble, not periodic healthy-stream telemetry.
void BluetoothContext::logInputStats(int index, uint64_t nowMs, bool force) {
    BluetoothLink &link = links[index];
    if (!app || !app->audio || (!force && nowMs < link.inStatsNextMs)) {
        return;
    }
    const uint64_t elapsedMs = nowMs > link.inStatsStartMs ? nowMs - link.inStatsStartMs : 0;
    link.inStatsNextMs = nowMs + BT_INPUT_STATS_INTERVAL_MS;
    link.inStatsStartMs = nowMs;
    const BluetoothInputStream &stream = app->audio->bluetoothInputs[index];
    const uint32_t underruns = stream.underruns.load(std::memory_order_relaxed);
    const uint32_t overflows = stream.overflows.load(std::memory_order_relaxed);
    const uint32_t dropped = stream.dropped.load(std::memory_order_relaxed);
    const uint32_t newUnderruns = underruns - link.inUnderrunsLogged;
    const uint32_t newOverflows = overflows - link.inOverflowsLogged;
    const uint32_t newDropped = dropped - link.inDroppedLogged;
    link.inUnderrunsLogged = underruns;
    link.inOverflowsLogged = overflows;
    link.inDroppedLogged = dropped;
    const uint32_t fill = stream.write.load(std::memory_order_relaxed) -
                          stream.read.load(std::memory_order_relaxed);
    const uint32_t arrivalRate = elapsedMs >= 1000U ? (uint32_t)((uint64_t)link.inFrames * 1000U / elapsedMs) : 0;
    const bool trouble = newUnderruns || newOverflows || newDropped || link.inLostPackets ||
                         link.inSkippedFrames || link.inDecodeErrors;
    char addressText[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(link.address, addressText, sizeof(addressText));
    if (trouble) {
        printf("[BT] [WARN] A2DP input from %s: rate=%u received=%u/s packets=%u max_gap=%ums gaps_over_60ms=%u "
               "lost_packets=%u skipped_by_sender=%u decode_errors=%u underruns=%u overflows=%u dropped_frames=%u "
               "buffered=%u target=%u\n",
               addressText, stream.sampleRate.load(std::memory_order_relaxed), arrivalRate, link.inPackets,
               link.inMaxGapMs, link.inLongGaps, link.inLostPackets, link.inSkippedFrames, link.inDecodeErrors,
               newUnderruns, newOverflows, newDropped, fill, stream.targetFill());
    }
    link.inPackets = 0;
    link.inFrames = 0;
    link.inMaxGapMs = 0;
    link.inLongGaps = 0;
    link.inLostPackets = 0;
    link.inSkippedFrames = 0;
    link.inDecodeErrors = 0;
}

void BluetoothContext::flushOutputPacket(int index) {
    BluetoothLink &link = links[index];
    if (link.outMediaFd < 0 || link.packetLength == 0) {
        return;
    }
    const ssize_t sent = send(link.outMediaFd, link.packet, link.packetLength, MSG_NOSIGNAL);
    if (sent == (ssize_t)link.packetLength) {
        link.packetLength = 0;
        return;
    }
    if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
        return;
    }
    closeOutputStream(index, sent < 0 ? strerror(errno) : "short A2DP media write");
}

void BluetoothContext::pumpOutputAudio(int index) {
    BluetoothLink &link = links[index];
    if (!link.used || !link.outUp || !app || !app->audio) {
        return;
    }
    BluetoothOutputStream &slot = app->audio->bluetoothOutputs[index];
    const uint64_t nowMs = bt::monotonicMs();
    const bool signalActive = slot.signalActive.load(std::memory_order_acquire) != 0;
    // Input commands share the signaling channel but do not block audio.
    const bool outputCommandPending = link.pendingSignal != 0 && !link.pendingInput;
    const bool commandSlotBusy = link.pendingSignal != 0 || link.commandWritePending;
    if (link.outStreaming) {
        if (signalActive) {
            link.silenceSinceMs = 0;
        } else if (link.silenceSinceMs == 0) {
            link.silenceSinceMs = nowMs;
        } else if (link.outSuspendSupported && !link.outSuspendRequested && !commandSlotBusy &&
                   link.packetLength == 0 &&
                   nowMs - link.silenceSinceMs >= BT_OUTPUT_SILENCE_THRESHOLD) {
            const uint8_t seid = (uint8_t)(link.remoteSinkSeid << 2);
            link.outSuspendRequested = true;
            if (!sendCommand(index, AVDTP_SUSPEND, &seid, 1, false)) {
                link.outSuspendRequested = false;
            }
            return;
        }
    } else {
        if (!signalActive || outputCommandPending || link.outStartRequested) {
            if (!signalActive && slot.active.load(std::memory_order_acquire)) {
                slot.setActive(false);
            }
            return;
        }
        if (!slot.active.load(std::memory_order_acquire)) {
            slot.setActive(true);
        }
    }

    if (nowMs >= link.statsNextMs) {
        const uint32_t dropped = slot.dropped.load(std::memory_order_relaxed);
        const uint32_t droppedThisSecond = dropped - link.droppedAtStats;
        link.droppedAtStats = dropped;
        const bool firstSample = link.statsNextMs == 0;
        link.statsNextMs = nowMs + 1000ULL;
        if (!firstSample && droppedThisSecond > SAMPLE_RATE / 20U) {
            closeOutputStream(index, "PCM output queue is overrunning; stopping glitchy stream");
            return;
        }
    }
    if (link.packetLength != 0 || !link.encoderReady || link.outMediaFd < 0 ||
        link.outSuspendRequested || outputCommandPending) {
        return;
    }
    const size_t codeSize = sbc_get_codesize(&link.encoder);
    if (codeSize == 0 || codeSize > 512 || (codeSize % 4) != 0) {
        closeOutputStream(index, "unsupported negotiated SBC frame size");
        return;
    }
    const uint32_t samplesPerFrame = (uint32_t)(codeSize / 4);
    const size_t sbcFrameLength = sbc_get_frame_length(&link.encoder);
    if (sbcFrameLength == 0 || sbcFrameLength >= link.mediaMtu - BT_RTP_SBC_HEADER_SIZE) {
        closeOutputStream(index, "SBC frame does not fit media MTU");
        return;
    }
    uint32_t frameCount = (link.mediaMtu - BT_RTP_SBC_HEADER_SIZE) / (uint32_t)sbcFrameLength;
    if (frameCount > BT_MAX_SBC_FRAMES_PER_PACKET) {
        frameCount = BT_MAX_SBC_FRAMES_PER_PACKET;
    }
    if (frameCount < 3U) {
        closeOutputStream(index, "media MTU permits fewer than three SBC frames per packet");
        return;
    }
    const uint32_t pcmFrames = samplesPerFrame * frameCount;
    if (!link.outStreaming) {
        if (commandSlotBusy || slot.available() < pcmFrames) {
            return;
        }
        const uint8_t seid = (uint8_t)(link.remoteSinkSeid << 2);
        link.outStartRequested = sendCommand(index, AVDTP_START, &seid, 1, false);
        return;
    }
    slot.updateRate(pcmFrames, nowMs);
    if (slot.available() < pcmFrames) {
        return;
    }
    int16_t pcm[BT_MAX_SBC_FRAMES_PER_PACKET * 128U * 2U];
    if (pcmFrames > BT_MAX_SBC_FRAMES_PER_PACKET * 128U || slot.resample(pcm, pcmFrames) != pcmFrames) {
        return;
    }

    uint8_t *packet = link.packet;
    const size_t capacity = link.mediaMtu < sizeof(link.packet) ? link.mediaMtu : sizeof(link.packet);
    packet[0] = 0x80;
    packet[1] = 96;
    writeBe16(packet + 2, (uint16_t)link.rtpSequence++);
    packet[4] = (uint8_t)(link.rtpTimestamp >> 24);
    packet[5] = (uint8_t)(link.rtpTimestamp >> 16);
    packet[6] = (uint8_t)(link.rtpTimestamp >> 8);
    packet[7] = (uint8_t)link.rtpTimestamp;
    packet[8] = 'A'; packet[9] = 'u'; packet[10] = 'd'; packet[11] = 'X';
    packet[12] = (uint8_t)frameCount;
    size_t packetLength = BT_RTP_SBC_HEADER_SIZE;
    for (uint32_t frame = 0; frame < frameCount; ++frame) {
        ssize_t written = 0;
        const ssize_t consumed = sbc_encode(&link.encoder,
                                            pcm + frame * (uint32_t)(codeSize / sizeof(int16_t)),
                                            codeSize,
                                            packet + packetLength,
                                            capacity - packetLength,
                                            &written);
        if (consumed != (ssize_t)codeSize || written <= 0 ||
            (size_t)written > capacity - packetLength) {
            closeOutputStream(index, "SBC encoding failed");
            return;
        }
        packetLength += (size_t)written;
    }
    link.rtpTimestamp += pcmFrames;
    link.packetLength = packetLength;
    flushOutputPacket(index);
}

void BluetoothContext::closeInputStream(int index, const char *reason) {
    BluetoothLink &link = links[index];
    const bool wasActive = link.inConfigured || link.inStreaming || link.inMediaFd >= 0;
    if (link.inStreaming) {
        logInputStats(index, bt::monotonicMs(), true);
    }
    closeFd(link.inMediaFd);
    if (app && app->audio) {
        app->audio->bluetoothInputs[index].stop();
    }
    if (link.decoderReady) {
        sbc_finish(&link.decoder);
        link.decoderReady = false;
    }
    if (link.pendingSignal != 0 && link.pendingInput) {
        link.pendingSignal = 0;
        link.commandLength = 0;
        link.commandWritePending = false;
    }
    link.inConfigured = false;
    link.inOpened = false;
    link.inStreaming = false;
    link.inRemoteActive = false;
    link.inMediaConnecting = false;
    link.inInitiateAtMs = 0;
    link.fragmentSize = 0;
    link.fragmentActive = false;
    link.wantInput = false;
    if (wasActive && reason && reason[0]) {
        char addressText[BT_ADDRESS_TEXT] = {};
        bt::formatAddress(link.address, addressText, sizeof(addressText));
        printf("[BT] [INFO] A2DP input from %s closed: %s\n", addressText, reason);
    }
}

void BluetoothContext::closeOutputStream(int index, const char *reason) {
    BluetoothLink &link = links[index];
    if (!link.wantInput && !link.inConfigured) {
        closeLink(index, reason);
        return;
    }
    if (app && app->audio) {
        BluetoothOutputStream &slot = app->audio->bluetoothOutputs[index];
        slot.setActive(false);
        slot.peer.store(0, std::memory_order_release);
    }
    closeFd(link.outMediaFd);
    if (link.encoderReady) {
        sbc_finish(&link.encoder);
        link.encoderReady = false;
    }
    if (link.pendingSignal != 0 && !link.pendingInput) {
        link.pendingSignal = 0;
        link.commandLength = 0;
        link.commandWritePending = false;
    }
    link.outStarting = false;
    link.outConfigured = false;
    link.outOpened = false;
    link.outUp = false;
    link.outStreaming = false;
    link.outMediaConnecting = false;
    link.outSuspendRequested = false;
    link.outStartRequested = false;
    link.packetLength = 0;
    link.silenceSinceMs = 0;
    link.statsNextMs = 0;
    link.wantOutput = false;
    link.connecting = false;
    link.deadlineMs = 0;
    if (reason && reason[0]) {
        char label[BT_NAME_MAX] = {};
        deviceLabel(link.address, label, sizeof(label));
        printf("[BT] [INFO] A2DP output to %s stopped: %s\n", label, reason);
        setMessage("%s output: %s", label, reason);
    }
}

void BluetoothContext::closeLink(int index, const char *reason, bool hold) {
    BluetoothLink &link = links[index];
    if (!link.used) {
        return;
    }
    const bt::Address address = link.address;
    const bool wasOutput = link.outUp || link.wantOutput;
    if (link.inStreaming) {
        logInputStats(index, bt::monotonicMs(), true);
    }
    closeFd(link.signalingFd);
    closeFd(link.inMediaFd);
    closeFd(link.outMediaFd);
    if (link.decoderReady) {
        sbc_finish(&link.decoder);
    }
    if (link.encoderReady) {
        sbc_finish(&link.encoder);
    }
    if (app && app->audio) {
        app->audio->bluetoothInputs[index].stop();
        BluetoothOutputStream &slot = app->audio->bluetoothOutputs[index];
        slot.setActive(false);
        slot.peer.store(0, std::memory_order_release);
    }
    resetLink(link);
    // A device that hung up its input meant it; outputs are re-dialed.
    if (hold && !wasOutput) {
        holdAutoConnect(address);
    }
    if (reason && reason[0]) {
        if (strcmp(reason, BT_USER_DISCONNECT_REASON) != 0) {
            scheduleAutoConnectRetry(address);
        }
        char label[BT_NAME_MAX] = {};
        deviceLabel(address, label, sizeof(label));
        printf("[BT] [INFO] %s disconnected: %s\n", label, reason);
        setMessage("%s: %s", label, reason);
    }
}

void BluetoothContext::closeAllLinks(const char *reason) {
    for (int i = 0; i < BT_MAX_LINKS; ++i) {
        closeLink(i, reason);
    }
}

void BluetoothContext::publishLinkStatus() {
    BluetoothLinkStatus snapshot[BT_MAX_LINKS] = {};
    for (int i = 0; i < BT_MAX_LINKS; ++i) {
        const BluetoothLink &link = links[i];
        if (!link.used) {
            continue;
        }
        const bool signalingUp = link.signalingFd >= 0 && !link.signalingConnecting;
        snapshot[i].used = true;
        snapshot[i].address = link.address;
        snapshot[i].connecting = link.connecting;
        snapshot[i].outputConnected = link.outUp;
        snapshot[i].outputStreaming = link.outUp && link.outStreaming;
        snapshot[i].inputConnected = signalingUp &&
            (link.inConfigured || (link.wantInput && !(link.wantOutput && !link.outUp)));
        snapshot[i].inputStreaming = link.inStreaming;
    }
    bool devicesChanged = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (int i = 0; i < BT_MAX_LINKS; ++i) {
            const BluetoothLinkStatus &before = linkStatus[i];
            const BluetoothLinkStatus &after = snapshot[i];
            if (before.used != after.used ||
                before.outputConnected != after.outputConnected ||
                before.inputConnected != after.inputConnected ||
                (after.used && !bt::sameAddress(before.address, after.address))) {
                devicesChanged = true;
            }
        }
        memcpy(linkStatus, snapshot, sizeof(linkStatus));
        if (request != BT_REQUEST_CONNECT) {
            connectQueued = false;
        }
    }
    if (devicesChanged) {
        audiox::audioHealth.devicesChanged();
    }
}

void BluetoothContext::runAutoConnect(uint64_t nowMs) {
    if (!adapterReady || nowMs < autoConnectCheckMs) {
        return;
    }
    autoConnectCheckMs = nowMs + BT_AUTOCONNECT_CHECK_MS;
    bool anyStream = false;
    bool freeLink = false;
    for (const BluetoothLink &link : links) {
        if (link.used && (link.connecting || link.signalingConnecting)) {
            // Paging one device at a time keeps the radio free for the rest.
            return;
        }
        if (!link.used) {
            freeLink = true;
            continue;
        }
        const bool signalingUp = link.signalingFd >= 0 && !link.signalingConnecting;
        if (signalingUp &&
            (link.inConfigured || (link.wantInput && !(link.wantOutput && !link.outUp)))) {
            // A page blocks the radio for seconds and drops incoming media,
            // even while the source is paused and about to resume. Absent
            // speakers reconnect to AudioX by themselves when switched on.
            return;
        }
        if (link.outUp || link.inStreaming) {
            anyStream = true;
        }
    }
    if (!freeLink) {
        return;
    }
    // Paging an absent device steals air time from running streams.
    if (anyStream && nowMs - lastAutoConnectMs < BT_AUTOCONNECT_STREAMING_GAP_MS) {
        return;
    }
    bt::Address target = {};
    bool found = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (scanning || pairing || deviceCount == 0) {
            return;
        }
        for (size_t offset = 0; offset < deviceCount; ++offset) {
            const size_t index = (autoConnectNextDevice + offset) % deviceCount;
            const BluetoothDevice &device = devices[index];
            if (!device.paired || !device.classic || !device.autoConnect || device.autoConnectHeld ||
                nowMs < device.autoConnectAtMs || findLink(device.address) >= 0) {
                continue;
            }
            target = device.address;
            autoConnectNextDevice = (index + 1U) % deviceCount;
            found = true;
            break;
        }
    }
    if (!found) {
        return;
    }
    char label[BT_NAME_MAX] = {};
    deviceLabel(target, label, sizeof(label));
    setMessage("Reconnecting to %s...", label);
    lastAutoConnectMs = nowMs;
    if (!beginConnect(target, true)) {
        scheduleAutoConnectRetry(target);
    }
}

void BluetoothContext::scheduleAutoConnectRetry(const bt::Address &address) {
    std::lock_guard<std::mutex> lock(mutex);
    BluetoothDevice *device = findDevice(address);
    if (!device) {
        return;
    }
    uint64_t backoff = device->autoConnectBackoffMs;
    if (backoff < BT_AUTOCONNECT_RETRY_MIN_MS) {
        backoff = BT_AUTOCONNECT_RETRY_MIN_MS;
    }
    device->autoConnectAtMs = bt::monotonicMs() + backoff;
    backoff *= 2U;
    device->autoConnectBackoffMs = backoff > BT_AUTOCONNECT_RETRY_MAX_MS ? BT_AUTOCONNECT_RETRY_MAX_MS : backoff;
}

void BluetoothContext::resetAutoConnectBackoff(const bt::Address &address) {
    std::lock_guard<std::mutex> lock(mutex);
    BluetoothDevice *device = findDevice(address);
    if (device) {
        device->autoConnectBackoffMs = BT_AUTOCONNECT_RETRY_MIN_MS;
        device->autoConnectAtMs = 0;
    }
}

void BluetoothContext::holdAutoConnect(const bt::Address &address) {
    std::lock_guard<std::mutex> lock(mutex);
    BluetoothDevice *device = findDevice(address);
    if (device) {
        device->autoConnectHeld = true;
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
            snprintf(message, sizeof(message), "%s does not support Classic Bluetooth; LE Audio is not supported", address);
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
    const int index = findLink(target);
    if (index >= 0) {
        holdAutoConnect(target);
        closeLink(index, "device forgotten");
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
    case BT_EVT_CONN_REQUEST: {
        if (paramLength < 10 || params[9] != BT_LINK_TYPE_ACL) {
            break;
        }
        const uint32_t classOfDevice = (uint32_t)params[6] |
                                       ((uint32_t)params[7] << 8) |
                                       ((uint32_t)params[8] << 16);
        const uint8_t majorClass = (uint8_t)((classOfDevice >> 8) & 0x1FU);
        printf("[BT] [INFO] connection request from %s (class 0x%06x)\n",
               addressText, (unsigned)classOfDevice);
        if (majorClass == BT_COD_MAJOR_PHONE) {
            inboundPhone = address;
            setDeviceRoles(address, 1, -1, "identified as a phone");
        }
        break;
    }
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
        if (!pairingAllowed(address)) {
            const uint8_t reject[] = {
                address.b[0], address.b[1], address.b[2], address.b[3], address.b[4], address.b[5],
                BT_PAIRING_NOT_ALLOWED,
            };
            bt::writeCommand(agentFd, BT_CMD_IO_CAPABILITY_NEG_REPLY, reject, sizeof(reject));
            printf("[BT] [WARN] rejected pairing from %s; pair new devices from AudioX\n", addressText);
            break;
        }
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
        if (!pairingAllowed(address)) {
            bt::writeCommand(agentFd, BT_CMD_PIN_CODE_NEG_REPLY, address.b, 6);
            printf("[BT] [WARN] rejected PIN pairing from %s; pair new devices from AudioX\n", addressText);
            break;
        }
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
                device->autoConnect = true;
            }
            if (device) {
                memcpy(device->linkKey, params + 6, 16);
                device->linkKeyType = params[22];
                if (!device->paired) {
                    device->autoConnect = true;
                }
                if (bt::sameAddress(address, inboundPhone)) {
                    device->audioSource = true;
                }
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

// AudioX is connectable, so only the device being paired from the web UI or a
// device that is already paired (refreshing its key) may pair.
bool BluetoothContext::pairingAllowed(const bt::Address &address) {
    std::lock_guard<std::mutex> lock(mutex);
    if (pairing && bt::sameAddress(address, pairTarget)) {
        return true;
    }
    const BluetoothDevice *device = findDevice(address);
    return device && device->paired;
}

// The link came up but the device refused the AVDTP channel, which is what a
// phone (an A2DP source with no sink endpoint) does when AudioX calls it. After
// repeated refusals, treat a device with unknown roles as an audio input so
// auto-connect stops calling it. Devices already known as outputs are kept.
void BluetoothContext::noteOutputChannelResult(const bt::Address &address, bool refused) {
    bool markInput = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        BluetoothDevice *device = findDevice(address);
        if (!device) {
            return;
        }
        if (!refused || device->audioSource || device->audioSink) {
            device->outputRefusals = 0;
            return;
        }
        if (device->outputRefusals < UINT8_MAX) {
            ++device->outputRefusals;
        }
        markInput = device->outputRefusals >= BT_OUTPUT_REFUSALS_BEFORE_INPUT;
        if (markInput) {
            device->outputRefusals = 0;
        }
    }
    if (markInput) {
        setDeviceRoles(address, 1, 0, "refused A2DP output repeatedly");
        char addressText[BT_ADDRESS_TEXT] = {};
        bt::formatAddress(address, addressText, sizeof(addressText));
        setMessage("%s refused audio output; treating it as an audio input", addressText);
    }
}

void BluetoothContext::setDeviceRoles(const bt::Address &address, int source, int sink,
                                      const char *reason) {
    if (bt::isZeroAddress(address)) {
        return;
    }
    bool changed = false;
    bool paired = false;
    bool finalSource = false;
    bool finalSink = false;
    {
        std::lock_guard<std::mutex> lock(mutex);
        BluetoothDevice *device = findDevice(address);
        if (!device) {
            return;
        }
        if (source >= 0 && device->audioSource != (source != 0)) {
            device->audioSource = source != 0;
            changed = true;
        }
        if (sink >= 0 && device->audioSink != (sink != 0)) {
            device->audioSink = sink != 0;
            changed = true;
        }
        paired = device->paired;
        finalSource = device->audioSource;
        finalSink = device->audioSink;
    }
    char addressText[BT_ADDRESS_TEXT] = {};
    bt::formatAddress(address, addressText, sizeof(addressText));
    if (changed) {
        printf("[BT] [INFO] %s %s; roles: %s%s%s\n", addressText, reason,
               finalSource ? "input" : "",
               finalSource && finalSink ? " and " : "",
               finalSink ? "output" : (finalSource ? " only, no output auto-connect" : "unknown"));
        if (paired) {
            savePaired();
            if (app && app->audio && app->audio->reloadRoutingGraph() == RET_ERR) {
                printf("[BT] [WARN] graph refresh after role change failed\n");
            }
        }
    }
}

size_t BluetoothContext::copyRoutingDevices(BluetoothRoutingDevice *out, size_t cap) const {
    if (!out || cap == 0) {
        return 0;
    }
    std::lock_guard<std::mutex> lock(mutex);
    size_t count = 0;
    for (size_t i = 0; i < deviceCount && count < cap; ++i) {
        const BluetoothDevice &device = devices[i];
        if (!device.paired || !device.classic) {
            continue;
        }
        const bool known = device.audioSource || device.audioSink;
        BluetoothRoutingDevice &entry = out[count++];
        entry.address = device.address;
        snprintf(entry.name, sizeof(entry.name), "%s", device.name);
        entry.input = !known || device.audioSource;
        entry.output = !known || device.audioSink;
        entry.connected = false;
        for (const BluetoothLinkStatus &status : linkStatus) {
            if (status.used && bt::sameAddress(status.address, device.address)) {
                entry.connected = status.inputConnected || status.outputConnected;
                break;
            }
        }
    }
    return count;
}

// Format: one device per line,
// "ADDRESS KEYHEX KEYTYPE NAME<TAB>AUTO=0|1[<TAB>SOURCE=0|1<TAB>SINK=0|1]".
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
        device.autoConnect = true;
        if (nameOffset > 0) {
            char *name = line + nameOffset;
            char *autoMarker = strstr(name, "\tAUTO=");
            bool hasAutoMarker = false;
            if (autoMarker) {
                unsigned enabled = 0;
                unsigned source = 0;
                unsigned sink = 0;
                int consumed = 0;
                if (sscanf(autoMarker, "\tAUTO=%u%n", &enabled, &consumed) == 1 && enabled <= 1) {
                    const char *rest = autoMarker + consumed;
                    int rolesConsumed = 0;
                    bool valid = rest[0] == '\0';
                    if (!valid &&
                        sscanf(rest, "\tSOURCE=%u\tSINK=%u%n", &source, &sink, &rolesConsumed) == 2 &&
                        source <= 1 && sink <= 1 && rest[rolesConsumed] == '\0') {
                        valid = true;
                    }
                    if (valid) {
                        device.autoConnect = enabled != 0;
                        device.audioSource = source != 0;
                        device.audioSink = sink != 0;
                        *autoMarker = '\0';
                        hasAutoMarker = true;
                    }
                }
            }
            if (hasAutoMarker && strcmp(name, "-") == 0) {
                name[0] = '\0';
            }
            snprintf(device.name, sizeof(device.name), "%s", name);
        }
        devices[deviceCount++] = device;
    }
    fclose(fp);
    printf("[BT] [INFO] loaded %zu paired device(s)\n", deviceCount);
}

bool BluetoothContext::savePaired() {
    std::lock_guard<std::mutex> fileLock(pairedFileMutex);
    if (mkdir(BT_STATE_DIR, 0755) != 0 && errno != EEXIST) {
        printf("[BT] [WARN] cannot create %s: %s\n", BT_STATE_DIR, strerror(errno));
        return false;
    }
    const char *tmpPath = BT_PAIRED_FILE ".tmp";
    FILE *fp = fopen(tmpPath, "w");
    if (!fp) {
        printf("[BT] [WARN] cannot write %s: %s\n", tmpPath, strerror(errno));
        return false;
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
            fprintf(fp, " %u %s\tAUTO=%u\tSOURCE=%u\tSINK=%u\n",
                    (unsigned)device.linkKeyType,
                    device.name[0] ? device.name : "-",
                    device.autoConnect ? 1U : 0U,
                    device.audioSource ? 1U : 0U,
                    device.audioSink ? 1U : 0U);
        }
    }
    bool saved = fflush(fp) == 0;
    if (!saved) {
        printf("[BT] [WARN] cannot flush %s: %s\n", tmpPath, strerror(errno));
    }
    if (fsync(fileno(fp)) != 0) {
        printf("[BT] [WARN] cannot sync %s: %s\n", tmpPath, strerror(errno));
        saved = false;
    }
    if (fclose(fp) != 0) {
        printf("[BT] [WARN] cannot close %s: %s\n", tmpPath, strerror(errno));
        saved = false;
    }
    if (!saved) {
        return false;
    }
    if (rename(tmpPath, BT_PAIRED_FILE) != 0) {
        printf("[BT] [WARN] cannot replace %s: %s\n", BT_PAIRED_FILE, strerror(errno));
        return false;
    }
    return true;
}
