#include "audio/context.hpp"

#include "audio/alsa_pcm.h"
#include "config/context.hpp"

#include <algorithm>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

struct ProbeItem {
    uint32_t card;
    uint32_t device;
    uint8_t hasPlayback;
    uint8_t hasCapture;
    char playbackPath[128];
    char capturePath[128];
    char nodeName[64];
};

static int parsePcmName(const char *name, uint32_t *card, uint32_t *device, uint8_t *playback) {
    if (!name || !card || !device || !playback) {
        return RET_ERR;
    }

    unsigned c = 0;
    unsigned d = 0;
    char suffix = 0;
    if (sscanf(name, "pcmC%uD%u%c", &c, &d, &suffix) != 3) {
        return RET_ERR;
    }

    if (suffix != 'p' && suffix != 'c') {
        return RET_ERR;
    }

    *card = (uint32_t)c;
    *device = (uint32_t)d;
    *playback = (suffix == 'p') ? 1 : 0;
    return RET_OK;
}

static bool pathExists(const char *path) {
    struct stat st;
    return (path && stat(path, &st) == 0);
}

static bool isUsbLikeNode(const char *nodeName) {
    if (!nodeName) {
        return false;
    }

    return strstr(nodeName, "USB") != nullptr || strstr(nodeName, "usb") != nullptr;
}

static void trimAsciiSpace(char *text) {
    if (!text || !text[0]) {
        return;
    }

    char *start = text;
    while (*start && isspace((unsigned char)*start)) {
        ++start;
    }

    if (start != text) {
        memmove(text, start, strlen(start) + 1);
    }

    size_t len = strlen(text);
    while (len > 0 && isspace((unsigned char)text[len - 1])) {
        text[len - 1] = '\0';
        --len;
    }
}

static int readProcTextFile(const char *path, char *out, size_t outSize) {
    if (!path || !out || outSize == 0) {
        return RET_ERR;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return RET_ERR;
    }

    ssize_t got = read(fd, out, outSize - 1);
    close(fd);
    if (got <= 0) {
        out[0] = '\0';
        return RET_ERR;
    }

    out[(size_t)got] = '\0';
    trimAsciiSpace(out);
    return RET_OK;
}

static int loadCardId(uint32_t card, char *out, size_t outSize) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/asound/card%u/id", (unsigned)card);
    return readProcTextFile(path, out, outSize);
}

static int loadPcmLabel(uint32_t card, uint32_t device, char *out, size_t outSize) {
    if (!out || outSize == 0) {
        return RET_ERR;
    }

    out[0] = '\0';
    FILE *fp = fopen("/proc/asound/pcm", "r");
    if (!fp) {
        return RET_ERR;
    }

    char line[256];
    while (fgets(line, sizeof(line), fp) != nullptr) {
        unsigned lineCard = 0;
        unsigned lineDevice = 0;
        char label[128] = {0};
        if (sscanf(line, "%u-%u: %127[^:]", &lineCard, &lineDevice, label) != 3) {
            continue;
        }
        if (lineCard != card || lineDevice != device) {
            continue;
        }

        trimAsciiSpace(label);
        snprintf(out, outSize, "%s", label);
        fclose(fp);
        return out[0] ? RET_OK : RET_ERR;
    }

    fclose(fp);
    return RET_ERR;
}

static int loadCardStreamLabel(uint32_t card, char *out, size_t outSize) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/asound/card%u/stream0", (unsigned)card);

    if (readProcTextFile(path, out, outSize) != RET_OK || !out[0]) {
        return RET_ERR;
    }

    char *colon = strstr(out, " : ");
    if (colon) {
        *colon = '\0';
    }
    trimAsciiSpace(out);
    return out[0] ? RET_OK : RET_ERR;
}

static bool containsTokenNoCase(const char *text, const char *token) {
    if (!text || !token || !token[0]) {
        return false;
    }

    size_t textLen = strlen(text);
    size_t tokenLen = strlen(token);
    if (tokenLen > textLen) {
        return false;
    }

    for (size_t i = 0; i + tokenLen <= textLen; ++i) {
        size_t j = 0;
        while (j < tokenLen) {
            unsigned char a = (unsigned char)text[i + j];
            unsigned char b = (unsigned char)token[j];
            if (tolower(a) != tolower(b)) {
                break;
            }
            ++j;
        }
        if (j == tokenLen) {
            return true;
        }
    }

    return false;
}

static bool isGenericUsbLabel(const char *text) {
    if (!text || !text[0]) {
        return false;
    }

    return strcasecmp(text, "Audio") == 0 ||
           strcasecmp(text, "USB Audio") == 0 ||
           strcasecmp(text, "AudioX") == 0;
}

static bool isUsbGadgetIdentity(const char *cardId,
                                const char *pcmLabel,
                                const char *streamLabel) {
    const char *fields[] = {cardId, pcmLabel, streamLabel};
    for (size_t i = 0; i < (sizeof(fields) / sizeof(fields[0])); ++i) {
        const char *field = fields[i];
        if (!field || !field[0]) {
            continue;
        }
        if (containsTokenNoCase(field, "audiox") ||
            containsTokenNoCase(field, "uac2") ||
            containsTokenNoCase(field, "gadget")) {
            return true;
        }
    }

    return false;
}

static void buildDisplayName(uint32_t card,
                             uint32_t device,
                             char *out,
                             size_t outSize,
                             uint8_t *isGadgetOut,
                             uint8_t *isUsbOut) {
    if (!out || outSize == 0) {
        return;
    }

    char cardId[64] = {0};
    char pcmLabel[128] = {0};
    char streamLabel[160] = {0};
    int haveCardId = (loadCardId(card, cardId, sizeof(cardId)) == RET_OK && cardId[0]);
    int havePcmLabel = (loadPcmLabel(card, device, pcmLabel, sizeof(pcmLabel)) == RET_OK && pcmLabel[0]);
    int haveStreamLabel = (loadCardStreamLabel(card, streamLabel, sizeof(streamLabel)) == RET_OK && streamLabel[0]);
    uint8_t isGadget = isUsbGadgetIdentity(haveCardId ? cardId : nullptr,
                                           havePcmLabel ? pcmLabel : nullptr,
                                           haveStreamLabel ? streamLabel : nullptr) ? 1U : 0U;
    uint8_t isUsb = isGadget;

    if (!isUsb && havePcmLabel && isUsbLikeNode(pcmLabel)) {
        isUsb = 1;
    }
    if (!isUsb && haveCardId && isUsbLikeNode(cardId)) {
        isUsb = 1;
    }
    if (!isUsb && haveStreamLabel && isUsbLikeNode(streamLabel)) {
        isUsb = 1;
    }

    if (haveStreamLabel && (isGadget || isGenericUsbLabel(haveCardId ? cardId : nullptr) || isGenericUsbLabel(havePcmLabel ? pcmLabel : nullptr))) {
        snprintf(out, outSize, "%s", streamLabel);
    } else if (haveCardId && havePcmLabel && strcasecmp(cardId, pcmLabel) != 0) {
        snprintf(out, outSize, "%s (%s)", cardId, pcmLabel);
    } else if (havePcmLabel) {
        snprintf(out, outSize, "%s", pcmLabel);
    } else if (haveCardId) {
        snprintf(out, outSize, "%s", cardId);
    } else {
        snprintf(out, outSize, "ALSA card %u device %u", (unsigned)card, (unsigned)device);
    }

    if (isGadgetOut) {
        *isGadgetOut = isGadget;
    }
    if (isUsbOut) {
        *isUsbOut = isUsb;
    }
}

static unsigned probePcmChannels(const char *path, int isCapture) {
    if (!path || !path[0]) {
        return 0;
    }

    const unsigned rates[] = {SAMPLE_RATE, AUDIO_INPUT_FALLBACK_RATE};
    const unsigned maxProbeChannels = 16;
    for (size_t rateIndex = 0; rateIndex < (sizeof(rates) / sizeof(rates[0])); ++rateIndex) {
        for (unsigned ch = maxProbeChannels; ch >= 1; --ch) {
            int fd = open(path, isCapture ? (O_RDONLY | O_NONBLOCK) : (O_WRONLY | O_NONBLOCK));
            if (fd < 0) {
                return 2;
            }

            snd_pcm_uframes_t periodFrames = 0;
            size_t frameBytes = 0;
            int ok = audio_pcm_configure_hw_fd(fd,
                                               path,
                                               rates[rateIndex],
                                               ch,
                                               isCapture ? AUDIO_CAPTURE_PERIOD_FRAMES : BUFFER_FRAMES,
                                               isCapture ? AUDIO_CAPTURE_PERIODS : 2U,
                                               &periodFrames,
                                               &frameBytes,
                                               1,
                                               0);
            close(fd);
            (void)periodFrames;
            (void)frameBytes;
            if (ok == 0) {
                return ch;
            }
        }
    }

    return 2;
}

} // namespace

size_t AudioContext::copyDeviceInfos(AudioDeviceInfo *out, size_t cap) const {
    if (!out || cap == 0) {
        return 0;
    }

    std::lock_guard<std::mutex> lock(devicesMutex);
    size_t copied = 0;
    for (const auto &it : devices) {
        if (copied >= cap) {
            break;
        }
        out[copied++] = it.second;
    }

    return copied;
}

int AudioContext::buildDevicesJson(char *out, size_t out_sz) const {
    if (!out || out_sz == 0) {
        return RET_ERR;
    }

    size_t used = 0;
    int n = snprintf(out + used, out_sz - used, "{\"ok\":true,\"devices\":[");
    if (n < 0 || (size_t)n >= out_sz - used) {
        return RET_ERR;
    }
    used += (size_t)n;

    std::lock_guard<std::mutex> lock(devicesMutex);
    size_t idx = 0;
    for (const auto &it : devices) {
        const AudioDeviceInfo &d = it.second;
        n = snprintf(
            out + used,
            out_sz - used,
            "%s{\"handle\":%u,\"generation\":%u,\"card\":%u,\"device\":%u,\"playback\":%u,\"capture\":%u,\"playbackChannels\":%u,\"captureChannels\":%u,\"usb\":%u,\"node\":\"%s\",\"path\":\"%s\",\"name\":\"%s\"}",
            (idx == 0) ? "" : ",",
            (unsigned)d.handle,
            (unsigned)d.generation,
            (unsigned)d.cardIndex,
            (unsigned)d.deviceIndex,
            (unsigned)d.hasPlayback,
            (unsigned)d.hasCapture,
            (unsigned)d.playbackChannels,
            (unsigned)d.captureChannels,
            (unsigned)d.isUsb,
            d.nodeName,
            d.devPath,
            d.displayName);
        if (n < 0 || (size_t)n >= out_sz - used) {
            return RET_ERR;
        }
        used += (size_t)n;
        idx++;
    }

    n = snprintf(out + used, out_sz - used, "]}");
    if (n < 0 || (size_t)n >= out_sz - used) {
        return RET_ERR;
    }

    return RET_OK;
}

int AudioContext::rescanDevices() {
    DIR *dir = opendir("/dev/snd");
    if (!dir) {
        std::lock_guard<std::mutex> lock(devicesMutex);
        devices.clear();
        pathToHandle.clear();
        deviceGeneration++;
        return RET_WARN;
    }

    std::unordered_map<uint64_t, ProbeItem> grouped;
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        uint32_t card = 0;
        uint32_t device = 0;
        uint8_t playback = 0;
        if (parsePcmName(ent->d_name, &card, &device, &playback) != RET_OK) {
            continue;
        }

        uint64_t key = ((uint64_t)card << 32ULL) | (uint64_t)device;
        auto found = grouped.find(key);
        if (found == grouped.end()) {
            ProbeItem p{};
            p.card = card;
            p.device = device;
            snprintf(p.nodeName, sizeof(p.nodeName), "card%u-device%u", (unsigned)card, (unsigned)device);
            if (playback) {
                p.hasPlayback = 1;
                snprintf(p.playbackPath,
                         sizeof(p.playbackPath),
                         "/dev/snd/pcmC%uD%up",
                         (unsigned)card,
                         (unsigned)device);
            } else {
                p.hasCapture = 1;
                snprintf(p.capturePath,
                         sizeof(p.capturePath),
                         "/dev/snd/pcmC%uD%uc",
                         (unsigned)card,
                         (unsigned)device);
            }
            grouped.emplace(key, p);
        } else {
            if (playback) {
                found->second.hasPlayback = 1;
                snprintf(found->second.playbackPath,
                         sizeof(found->second.playbackPath),
                         "/dev/snd/pcmC%uD%up",
                         (unsigned)card,
                         (unsigned)device);
            } else {
                found->second.hasCapture = 1;
                snprintf(found->second.capturePath,
                         sizeof(found->second.capturePath),
                         "/dev/snd/pcmC%uD%uc",
                         (unsigned)card,
                         (unsigned)device);
            }
        }
    }
    closedir(dir);

    ConfigData gadgetConfig = {};
    int haveGadgetConfig = 0;
    if (app && app->config) {
        gadgetConfig = app->config->readConfigFile();
        haveGadgetConfig = 1;
    }

    std::unordered_map<AudioHandle, AudioDeviceInfo> next;
    std::unordered_map<std::string, AudioHandle> nextPath;

    for (const auto &it : grouped) {
        const ProbeItem &p = it.second;
        const char *preferredPath = p.hasPlayback ? p.playbackPath : p.capturePath;

        AudioHandle handle = 0;
        auto oldPath = pathToHandle.find(std::string(preferredPath));
        if (oldPath != pathToHandle.end()) {
            handle = oldPath->second;
        } else {
            handle = nextHandle++;
            if (nextHandle == 0) {
                nextHandle = 1;
            }
        }

        AudioDeviceInfo info{};
        info.handle = handle;
        info.generation = deviceGeneration + 1;
        info.cardIndex = p.card;
        info.deviceIndex = p.device;
        info.hasPlayback = p.hasPlayback;
        info.hasCapture = p.hasCapture;
        info.isUsb = 0;
        info.isGadget = 0;
        info.playbackChannels = (uint8_t)(p.hasPlayback ? probePcmChannels(p.playbackPath, 0) : 0);
        info.captureChannels = (uint8_t)(p.hasCapture ? probePcmChannels(p.capturePath, 1) : 0);
        snprintf(info.nodeName, sizeof(info.nodeName), "%s", p.nodeName);
        snprintf(info.devPath, sizeof(info.devPath), "%s", preferredPath);
        buildDisplayName(p.card,
                         p.device,
                         info.displayName,
                         sizeof(info.displayName),
                         &info.isGadget,
                         &info.isUsb);
        if (info.isGadget && haveGadgetConfig) {
            if (info.hasPlayback) {
                info.playbackChannels = (uint8_t)gadgetConfig.playbackChannels;
            }
            if (info.hasCapture) {
                info.captureChannels = (uint8_t)gadgetConfig.captureChannels;
            }
        }

        if (!pathExists(info.devPath)) {
            continue;
        }

        next.emplace(handle, info);
        nextPath.emplace(std::string(info.devPath), handle);
    }

    {
        std::lock_guard<std::mutex> lock(devicesMutex);
        devices.swap(next);
        pathToHandle.swap(nextPath);
        deviceGeneration++;
    }

    return RET_OK;
}
