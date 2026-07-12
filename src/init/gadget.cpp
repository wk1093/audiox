#include "init.hpp"
#include "context.hpp"
#include "config/context.hpp"
#include "system.hpp"

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
    if (cfg.sampleRate == 0) {
        cfg.sampleRate = SAMPLE_RATE;
    }
    if (cfg.playbackChannels == 0) {
        cfg.playbackChannels = 2;
    }
    if (cfg.captureChannels == 0) {
        cfg.captureChannels = 2;
    }
    if (cfg.sampleSize == 0) {
        cfg.sampleSize = 2;
    }


    uint32_t p_chmask = usbAudioChannelMask(cfg.playbackChannels);
    uint32_t c_chmask = usbAudioChannelMask(cfg.captureChannels);

    if (p_chmask == 0 || c_chmask == 0) {
        printf("[INIT] [WARN] invalid channel mask for USB audio gadget: playback=%u capture=%u\n",
               cfg.playbackChannels,
               cfg.captureChannels);
        return RET_WARN;
    }

    if (cfg.sampleRate < 8000 || cfg.sampleRate > 192000) {
        printf("[INIT] [WARN] invalid sample rate for USB audio gadget: %u\n", cfg.sampleRate);
        return RET_WARN;
    }

    if (cfg.sampleSize == 0 || cfg.sampleSize > 4) {
        printf("[INIT] [WARN] invalid sample size for USB audio gadget: %u\n", cfg.sampleSize);
        return RET_WARN;
    }

    if (ensureDir(GADGET_ROOT, 0755) != RET_OK) return RET_ERR;

    if (writeSysNode(GADGET_ROOT "/idVendor", "0x6666\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/idProduct", "0x0ad0\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/bcdDevice", "0x0100\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/bcdUSB", "0x0200\n") != RET_OK) return RET_ERR;

    if (ensureDir(GADGET_ROOT "/strings/0x409", 0755) != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/strings/0x409/serialnumber", "AUDIOX0001\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/strings/0x409/manufacturer", "AudioX\n") != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/strings/0x409/product", "AudioX\n") != RET_OK) return RET_ERR;

    if (ensureDir(GADGET_ROOT "/configs/c.1", 0755) != RET_OK) return RET_ERR;
    if (ensureDir(GADGET_ROOT "/configs/c.1/strings/0x409", 0755) != RET_OK) return RET_ERR;
    if (writeSysNode(GADGET_ROOT "/configs/c.1/strings/0x409/configuration", "UAC2 Audio Stream\n") != RET_OK) return RET_ERR;

    if (ensureDir(GADGET_UAC2_FUNC, 0755) != RET_OK) return RET_ERR;

    if (writeSysNodeU32(GADGET_UAC2_FUNC "/p_chmask", p_chmask) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/c_chmask", c_chmask) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/p_srate", cfg.sampleRate) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/c_srate", cfg.sampleRate) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/p_ssize", cfg.sampleSize) != RET_OK) return RET_ERR;
    if (writeSysNodeU32(GADGET_UAC2_FUNC "/c_ssize", cfg.sampleSize) != RET_OK) return RET_ERR;

    if (symlink(GADGET_UAC2_FUNC, GADGET_CONFIG_LINK) < 0 && errno != EEXIST) {
        printf("[INIT] [ERR] Symlink layout assignment failed: %s\n", strerror(errno));
        return RET_ERR;
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