#include "init.hpp"
#include "context.hpp"
#include "config/context.hpp"
#include "system.hpp"
#include "audio/gadget_volume.hpp"
#include "audio/context.hpp"

#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>

static inline uint32_t usbAudioChannelMask(uint32_t channels) {
    if (channels == 0 || channels > 16) {
        return 0;
    }
    return (1u << channels) - 1u;
}

static inline uint32_t usbAudioChannelsFromMask(uint32_t mask) {
    if (mask == 0) {
        return 0;
    }

    uint32_t channels = 0;
    uint32_t m = mask;
    while ((m & 1u) != 0u) {
        ++channels;
        m >>= 1;
    }

    if (m != 0u || channels > 16U) {
        return 0;
    }
    return channels;
}

namespace {

static int validateAudioConfig(const ConfigData *cfg) {
    if (!cfg) {
        return RET_ERR;
    }

    uint32_t p_chmask = usbAudioChannelMask(cfg->playbackChannels);
    uint32_t c_chmask = usbAudioChannelMask(cfg->captureChannels);

    if (p_chmask == 0 || c_chmask == 0) {
        printf("[INIT] [WARN] invalid channel mask for USB audio gadget: playback=%u capture=%u\n",
               cfg->playbackChannels,
               cfg->captureChannels);
        return RET_WARN;
    }

    if (cfg->sampleRate < 8000 || cfg->sampleRate > 192000) {
        printf("[INIT] [WARN] invalid sample rate for USB audio gadget: %u\n", cfg->sampleRate);
        return RET_WARN;
    }

    if (cfg->sampleSize == 0 || cfg->sampleSize > 4) {
        printf("[INIT] [WARN] invalid sample size for USB audio gadget: %u\n", cfg->sampleSize);
        return RET_WARN;
    }

    return RET_OK;
}

// Windows keys device instances and cached audio endpoint names on the USB
// serial, so a per-board serial keeps boards distinct on one host.
static void usbSerialNumber(char *out, size_t outSize) {
    char raw[64] = {};
    int fd = open(BOARD_SERIAL_PATH, O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        ssize_t n = read(fd, raw, sizeof(raw) - 1);
        close(fd);
        raw[n > 0 ? n : 0] = '\0';
    }

    char board[32] = {};
    size_t len = 0;
    for (const char *p = raw; *p && len < sizeof(board) - 1; ++p) {
        if ((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F')) {
            board[len++] = (*p >= 'a' && *p <= 'f') ? (char)(*p - 'a' + 'A') : *p;
        }
    }

    if (len == 0) {
        printf("[INIT] [WARN] board serial unavailable; using USB serial %s\n", AUDIOX_USB_SERIAL_FALLBACK);
        snprintf(out, outSize, "%s\n", AUDIOX_USB_SERIAL_FALLBACK);
        return;
    }
    snprintf(out, outSize, "%s%s\n", AUDIOX_USB_SERIAL_PREFIX, board);
}

static int applyAudioConfig(const ConfigData *cfg) {
    if (!cfg) {
        return RET_ERR;
    }

    uint32_t p_chmask = usbAudioChannelMask(cfg->playbackChannels);
    uint32_t c_chmask = usbAudioChannelMask(cfg->captureChannels);

    if (ensureDir(GADGET_ROOT, 0755) != RET_OK) return RET_ERR;

    if (writeSysNode(GADGET_ROOT "/idVendor", "0x6666\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/idProduct", "0x0ad0\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/bcdDevice", "0x0100\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/bcdUSB", "0x0200\n") != RET_OK) return RET_ERR;

    if (ensureDir(GADGET_ROOT "/strings/0x409", 0755) != RET_OK) return RET_ERR;
    char serialNumber[64];
    usbSerialNumber(serialNumber, sizeof(serialNumber));
    if (writeSysNode(GADGET_ROOT "/strings/0x409/serialnumber", serialNumber) != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/strings/0x409/manufacturer", "AudioX\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/strings/0x409/product", "AudioX\n") != RET_OK) return RET_ERR;

    if (ensureDir(GADGET_ROOT "/configs/c.1", 0755) != RET_OK) return RET_ERR;
    if (ensureDir(GADGET_ROOT "/configs/c.1/strings/0x409", 0755) != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/configs/c.1/strings/0x409/configuration", "UAC2 Audio Stream\n") != RET_OK) return RET_ERR;

    if (ensureDir(GADGET_UAC2_FUNC, 0755) != RET_OK) return RET_ERR;

    if (writeSysNodeU32(GADGET_UAC2_FUNC "/p_chmask", p_chmask) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/c_chmask", c_chmask) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/p_srate", cfg->sampleRate) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/c_srate", cfg->sampleRate) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/p_ssize", cfg->sampleSize) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/c_ssize", cfg->sampleSize) != RET_OK) return RET_ERR;

    // Windows names the audio endpoints from these strings; the kernel default
    // function_name is "Source/Sink". p_* is device-to-host (host capture).
    struct NameSetting { const char *attr; const char *value; };
    static const NameSetting names[] = {
        {"function_name", AUDIOX_USB_AUDIO_NAME "\n"},
        {"if_ctrl_name", AUDIOX_USB_AUDIO_NAME " Control\n"},
        {"p_it_name", AUDIOX_USB_AUDIO_NAME " Capture\n"},
        {"p_ot_name", AUDIOX_USB_AUDIO_NAME " Capture\n"},
        {"p_it_ch_name", AUDIOX_USB_AUDIO_NAME " Capture Channels\n"},
        {"p_fu_vol_name", AUDIOX_USB_AUDIO_NAME " Capture Volume\n"},
        {"c_it_name", AUDIOX_USB_AUDIO_NAME " Playback\n"},
        {"c_ot_name", AUDIOX_USB_AUDIO_NAME " Playback\n"},
        {"c_it_ch_name", AUDIOX_USB_AUDIO_NAME " Playback Channels\n"},
        {"c_fu_vol_name", AUDIOX_USB_AUDIO_NAME " Playback Volume\n"},
    };
    for (const NameSetting &name : names) {
        char path[256];
        snprintf(path, sizeof(path), GADGET_UAC2_FUNC "/%s", name.attr);
        if (writeSysNode(path, name.value) != RET_OK) {
            printf("[INIT] [WARN] setting USB audio string %s failed: %s\n", path, strerror(errno));
        }
    }

    static const char *directions[] = {"p", "c"};
    for (const char *direction : directions) {
        char path[256];
        struct Setting { const char *name; int value; };
        const Setting settings[] = {
            {"mute_present", 1}, {"volume_present", 1},
            {"volume_min", audiox::usb::kVolumeMin},
            {"volume_max", audiox::usb::kVolumeMax},
            {"volume_res", audiox::usb::kVolumeResolution},
        };
        for (const Setting &setting : settings) {
            snprintf(path, sizeof(path), GADGET_UAC2_FUNC "/%s_%s", direction, setting.name);
            char value[32];
            snprintf(value, sizeof(value), "%d\n", setting.value);
            if (writeSysNode(path, value) != RET_OK) {
                printf("[INIT] [ERR] configuring USB audio control %s failed: %s\n", path, strerror(errno));
                return RET_ERR;
            }
        }
    }

    if (symlink(GADGET_UAC2_FUNC, GADGET_CONFIG_LINK) < 0 && errno != EEXIST) {
        printf("[INIT] [ERR] Symlink layout assignment failed: %s\n", strerror(errno));
        return RET_ERR;
    }

    printf("[INIT] [INFO] USB audio gadget config applied: playback=%u capture=%u rate=%u ssize=%u\n",
           cfg->playbackChannels,
           cfg->captureChannels,
           cfg->sampleRate,
           cfg->sampleSize);
    return RET_OK;
}


static int readCurrentAudioConfig(ConfigData *cfg) {
    if (!cfg) {
        return RET_ERR;
    }

    memset(cfg, 0, sizeof(*cfg));
    cfg->sampleRate = SAMPLE_RATE;
    cfg->playbackChannels = 2;
    cfg->captureChannels = 2;
    cfg->sampleSize = 2;

    uint32_t value = 0;
    if (readSysNodeU32(GADGET_UAC2_FUNC "/p_chmask", &value) == RET_OK) {
        uint32_t channels = usbAudioChannelsFromMask(value);
        if (channels > 0) {
            cfg->playbackChannels = channels;
        }
    }
    if (readSysNodeU32(GADGET_UAC2_FUNC "/c_chmask", &value) == RET_OK) {
        uint32_t channels = usbAudioChannelsFromMask(value);
        if (channels > 0) {
            cfg->captureChannels = channels;
        }
    }
    if (readSysNodeU32(GADGET_UAC2_FUNC "/c_srate", &value) == RET_OK && value > 0) {
        cfg->sampleRate = value;
    } else if (readSysNodeU32(GADGET_UAC2_FUNC "/p_srate", &value) == RET_OK && value > 0) {
        cfg->sampleRate = value;
    }
    if (readSysNodeU32(GADGET_UAC2_FUNC "/c_ssize", &value) == RET_OK && value > 0) {
        cfg->sampleSize = value;
    } else if (readSysNodeU32(GADGET_UAC2_FUNC "/p_ssize", &value) == RET_OK && value > 0) {
        cfg->sampleSize = value;
    }

    return RET_OK;
}

} // namespace

int setupAudioGadget(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }

    if (!context->config) {
        printf("[INIT] [WARN] config store unavailable during USB audio gadget setup\n");
        return RET_WARN;
    }

    ConfigData cfg = context->config->readConfigFile();
    int valid = validateAudioConfig(&cfg);
    if (valid != RET_OK) {
        return valid;
    }

    return applyAudioConfig(&cfg);
}

int reloadAudioGadget(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }
    if (!context->config) {
        printf("[INIT] [WARN] config store unavailable during USB audio gadget reload\n");
        return RET_WARN;
    }

    ConfigData nextCfg = {};
    int configSourceIsStaging = 0;
    int stagedRc = context->config->readStagingConfigFile(&nextCfg);
    if (stagedRc == RET_OK) {
        configSourceIsStaging = 1;
    } else {
        nextCfg = context->config->readConfigFile();
    }
    int valid = validateAudioConfig(&nextCfg);
    if (valid != RET_OK) {
        return valid;
    }
    if (context->audio) {
        context->audio->gadgetVolume.reset();
    }

    ConfigData prevCfg;
    (void)readCurrentAudioConfig(&prevCfg);

    int unbound = 0;
    if (writeSysNode(GADGET_UDC_NODE, "\n") == RET_OK) {
        unbound = 1;
    } else if (writeSysNode(GADGET_UDC_NODE, "") == RET_OK) {
        unbound = 1;
    }
    if (unbound) {
        usleep(20000);
    } else {
        printf("[INIT] [WARN] USB gadget unbind failed before reload; attempting live update\n");
    }

    if (unlink(GADGET_CONFIG_LINK) < 0 && errno != ENOENT) {
        printf("[INIT] [WARN] failed to unlink audio gadget config link: %s\n", strerror(errno));
    }

    int ret = applyAudioConfig(&nextCfg);
    if (ret != RET_OK) {
        printf("[INIT] [WARN] USB audio gadget reload failed; restoring previous config\n");
        (void)applyAudioConfig(&prevCfg);
        int restoreRc = bindUsbGadget(context);
        (void)restoreRc;
        return ret;
    }

    ret = bindUsbGadget(context);
    if (ret != RET_OK) {
        printf("[INIT] [WARN] USB gadget rebind failed after reload; restoring previous config\n");
        (void)unlink(GADGET_CONFIG_LINK);
        (void)applyAudioConfig(&prevCfg);
        int restoreRc = bindUsbGadget(context);
        (void)restoreRc;
        return ret;
    }

    if (configSourceIsStaging) {
        if (context->config->writeConfigFile(&nextCfg) < 0) {
            printf("[INIT] [WARN] gadget reload succeeded but failed to commit staged config\n");
            return RET_ERR;
        }
        if (unlink(CONFIG_STAGING_FILE_PATH) < 0 && errno != ENOENT) {
            printf("[INIT] [WARN] failed to remove %s after successful reload: %s\n",
                   CONFIG_STAGING_FILE_PATH,
                   strerror(errno));
        }
    }

    return RET_OK;
}

int setupNetworkGadget(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }

    if (ensureDir(GADGET_ROOT, 0755) != RET_OK) return RET_ERR;
    if (ensureDir(GADGET_ROOT "/configs", 0755) != RET_OK) return RET_ERR;
    if (ensureDir(GADGET_ROOT "/configs/c.1", 0755) != RET_OK) return RET_ERR;
    if (ensureDir(GADGET_NETWORK_FUNC, 0755) != RET_OK) return RET_ERR;

    if (writeSysNode(GADGET_NETWORK_FUNC "/dev_addr", "02:00:00:00:00:02\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_NETWORK_FUNC "/host_addr", "02:00:00:00:00:01\n") != RET_OK) return RET_ERR;

    if (symlink(GADGET_NETWORK_FUNC, GADGET_NETWORK_LINK) < 0 && errno != EEXIST) {
        printf("[INIT] [ERR] Symlink layout assignment failed: %s\n", strerror(errno));
        return RET_ERR;
    }

    return RET_OK;
}

int bindUsbGadget(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }

    if (writeSysNode(GADGET_UDC_NODE, GADGET_UDC_NAME) != RET_OK) {
        printf("[INIT] [ERR] failed to bind USB gadget to UDC '%s'\n", GADGET_UDC_NAME);
        return RET_ERR;
    }

    return RET_OK;
}
