#include "audio/context.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "config/context.hpp"

AudioContext::AudioContext(Audiox *context) : app(context) {
    if (app) {
        app->audio = this;
    }
}

int AudioContext::setupThreads() {
    // TODO
    return RET_WARN;
}

int AudioContext::triggerSfx(const char *sfxPath) {
    if (!sfxPath || !sfxPath[0]) {
        return RET_ERR;
    }

    if (strncmp(sfxPath, SFX_ROOT_DIR "/", strlen(SFX_ROOT_DIR) + 1) != 0) {
        printf("[AUDIO] [WARN] refusing SFX outside %s: %s\n", SFX_ROOT_DIR, sfxPath);
        return RET_ERR;
    }

    struct stat st;
    if (stat(sfxPath, &st) < 0) {
        printf("[AUDIO] [WARN] mapped SFX missing: %s\n", sfxPath);
        return RET_WARN;
    }

    // Placeholder until audio engine routing/playback is fully ported.
    printf("[AUDIO] [WARN] SFX trigger requested but playback is not implemented yet: %s\n", sfxPath);
    return RET_WARN;
}

void AudioContext::poll() {
    // TODO
}
