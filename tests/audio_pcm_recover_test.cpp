#include "audio/alsa_pcm.h"

#include <cstdio>
#include <initializer_list>

struct _snd_pcm {
    int token;
};

namespace {

snd_pcm_t* recoveredPcm = nullptr;
int recoveredError = 0;
int recoverySilent = 0;
int recoveryResult = 0;
unsigned recoveryCalls = 0;

}

extern "C" int snd_pcm_recover(snd_pcm_t* pcm, int err, int silent) {
    recoveredPcm = pcm;
    recoveredError = err;
    recoverySilent = silent;
    ++recoveryCalls;
    return recoveryResult;
}

int main() {
    if (audio_pcm_recover(nullptr, -EPIPE, "test", "playback") != -EINVAL ||
        recoveryCalls != 0) {
        std::fprintf(stderr, "null PCM handle reached ALSA recovery\n");
        return 1;
    }

    snd_pcm_t pcm = {};
    for (const int error : {-EPIPE, -ESTRPIPE, -EINTR}) {
        if (audio_pcm_recover(&pcm, error, "test", "playback") != 0 ||
            recoveredPcm != &pcm || recoveredError != error || recoverySilent != 1) {
            std::fprintf(stderr, "PCM recovery did not preserve the error or silence duplicate logs\n");
            return 1;
        }
    }

    recoveryResult = -EIO;
    if (audio_pcm_recover(&pcm, -EPIPE, "test", "playback") != -EIO ||
        recoveryCalls != 4 || recoverySilent != 1) {
        std::fprintf(stderr, "PCM recovery failure was not propagated\n");
        return 1;
    }

    if (!audio_pcm_should_log_underrun_count(1) ||
        !audio_pcm_should_log_underrun_count(10) ||
        !audio_pcm_should_log_underrun_count(250) ||
        audio_pcm_should_log_underrun_count(2) ||
        audio_pcm_should_log_underrun_count(251)) {
        std::fprintf(stderr, "underrun counter logging is not rate limited\n");
        return 1;
    }

    std::puts("audio PCM recovery tests passed");
    return 0;
}