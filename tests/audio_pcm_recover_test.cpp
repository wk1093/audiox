#include "audio/alsa_pcm.h"

#include <cstdio>
#include <initializer_list>
#include <cassert>

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
    audiox::AudioHealth health(0);
    for (uint64_t t : {1000, 2000, 3000, 4000, 5000}) assert(!health.recordXrun(t));
    assert(health.warningAt() == 0 && health.total() == 5);
    for (uint64_t t : {30000, 31000, 32000, 33000}) assert(!health.recordXrun(t));
    assert(health.recordXrun(34000)); // Exactly five eligible events, not six.
    assert(health.warningActive(34000) && health.warningActive(64000));
    assert(!health.warningActive(64001));
    for (uint64_t t = 35000; t < 154000; t += 1000) assert(!health.recordXrun(t));
    assert(health.recordXrun(154000)); // Cooldown boundary is inclusive.
    health.devicesChanged(155000);
    assert(!health.warningActive(155000));
    for (uint64_t t = 155000; t < 185000; t += 1000) assert(!health.recordXrun(t));
    for (uint64_t t : {274000, 285000, 295000, 305000}) assert(!health.recordXrun(t));
    assert(!health.recordXrun(314001)); // Five spread beyond 30 seconds must not warn.
    assert(health.recordXrun(314002));
    audiox::AudioHealth changed(0);
    for (uint64_t t : {30000, 31000, 32000, 33000}) assert(!changed.recordXrun(t));
    changed.devicesChanged(34000);
    for (uint64_t t : {64000, 65000, 66000, 67000}) assert(!changed.recordXrun(t));
    assert(changed.recordXrun(68000)); // Pre-change events cannot count toward a warning.
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
    assert(audiox::audioHealth.total() == 2); // ESTRPIPE and EINTR are not X-runs.

    std::puts("audio PCM recovery tests passed");
    return 0;
}