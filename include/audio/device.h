#ifndef AUDIO_DEVICE_H
#define AUDIO_DEVICE_H

#include "audio/types.h"

static inline void audio_endpoint_reset(audio_endpoint_t *ep, audio_device_role_t role) {
    if (!ep) {
        return;
    }

    memset(ep, 0, sizeof(*ep));
    ep->role = role;
    ep->kind = AUDIO_DEVICE_KIND_UNKNOWN;
    ep->transport = AUDIO_DEVICE_TRANSPORT_UNKNOWN;
    ep->backend = AUDIO_DEVICE_BACKEND_NONE;
    ep->card_index = -1;
}

static inline audio_device_kind_t audio_guess_kind(audio_device_role_t role,
                                                   int is_usb,
                                                   int card_index) {
    if (role == AUDIO_DEVICE_ROLE_CAPTURE && is_usb) {
        return AUDIO_DEVICE_KIND_USB_MIC;
    }

    if (role == AUDIO_DEVICE_ROLE_PLAYBACK && !is_usb && card_index == 0) {
        return AUDIO_DEVICE_KIND_USB_OTG_GADGET;
    }

    if (!is_usb && card_index >= 0) {
        return AUDIO_DEVICE_KIND_PCM3168;
    }

    return AUDIO_DEVICE_KIND_UNKNOWN;
}

static inline audio_device_transport_t audio_guess_transport(int is_usb,
                                                             audio_device_kind_t kind) {
    if (is_usb) {
        return AUDIO_DEVICE_TRANSPORT_USB;
    }

    if (kind == AUDIO_DEVICE_KIND_PCM3168) {
        return AUDIO_DEVICE_TRANSPORT_I2S;
    }

    if (kind == AUDIO_DEVICE_KIND_USB_OTG_GADGET) {
        return AUDIO_DEVICE_TRANSPORT_USB_OTG;
    }

    return AUDIO_DEVICE_TRANSPORT_SOC;
}

static inline void audio_endpoint_assign(audio_endpoint_t *ep,
                                         audio_device_role_t role,
                                         const char *path,
                                         int card_index,
                                         int is_usb,
                                         unsigned int channels,
                                         unsigned int sample_rate,
                                         unsigned int sample_size_bytes) {
    if (!ep) {
        return;
    }

    audio_endpoint_reset(ep, role);
    ep->backend = AUDIO_DEVICE_BACKEND_ALSA;
    ep->card_index = card_index;
    ep->is_usb = is_usb ? 1 : 0;
    ep->channels = (uint8_t)channels;
    ep->sample_size_bytes = (uint8_t)sample_size_bytes;
    ep->sample_rate = sample_rate;

    if (path && path[0]) {
        size_t n = strnlen(path, sizeof(ep->path) - 1);
        memcpy(ep->path, path, n);
        ep->path[n] = '\0';
    }

    ep->kind = audio_guess_kind(role, is_usb, card_index);
    ep->transport = audio_guess_transport(is_usb, ep->kind);
}

#endif
