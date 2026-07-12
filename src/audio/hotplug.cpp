#include "audio/context.hpp"

#include <algorithm>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

namespace {

struct ProbeItem {
    uint32_t card;
    uint32_t device;
    uint8_t hasPlayback;
    uint8_t hasCapture;
    char devPath[128];
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

    // Heuristic for now: cards >= 1 or explicit USB in card node naming.
    return strstr(nodeName, "USB") != nullptr || strstr(nodeName, "usb") != nullptr;
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
            "%s{\"handle\":%u,\"generation\":%u,\"card\":%u,\"device\":%u,\"playback\":%u,\"capture\":%u,\"usb\":%u,\"node\":\"%s\",\"path\":\"%s\",\"name\":\"%s\"}",
            (idx == 0) ? "" : ",",
            (unsigned)d.handle,
            (unsigned)d.generation,
            (unsigned)d.cardIndex,
            (unsigned)d.deviceIndex,
            (unsigned)d.hasPlayback,
            (unsigned)d.hasCapture,
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
            snprintf(p.devPath,
                     sizeof(p.devPath),
                     "/dev/snd/pcmC%uD%u%c",
                     (unsigned)card,
                     (unsigned)device,
                     playback ? 'p' : 'c');
            snprintf(p.nodeName, sizeof(p.nodeName), "card%u-device%u", (unsigned)card, (unsigned)device);
            p.hasPlayback = playback ? 1 : 0;
            p.hasCapture = playback ? 0 : 1;
            grouped.emplace(key, p);
        } else {
            if (playback) {
                found->second.hasPlayback = 1;
            } else {
                found->second.hasCapture = 1;
            }
        }
    }
    closedir(dir);

    std::unordered_map<AudioHandle, AudioDeviceInfo> next;
    std::unordered_map<std::string, AudioHandle> nextPath;

    for (const auto &it : grouped) {
        const ProbeItem &p = it.second;

        AudioHandle handle = 0;
        auto oldPath = pathToHandle.find(std::string(p.devPath));
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
        info.isUsb = isUsbLikeNode(p.nodeName) ? 1 : 0;
        snprintf(info.nodeName, sizeof(info.nodeName), "%s", p.nodeName);
        snprintf(info.devPath, sizeof(info.devPath), "%s", p.devPath);
        snprintf(info.displayName, sizeof(info.displayName), "ALSA card %u device %u", (unsigned)p.card, (unsigned)p.device);

        // If the node disappeared between scan and now, skip it to keep model clean.
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
