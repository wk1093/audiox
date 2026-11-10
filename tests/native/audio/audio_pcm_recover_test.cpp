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
snd_pcm_state_t recoveryPcmState = SND_PCM_STATE_PREPARED;
int recoveryStartResult = 0;
unsigned recoveryStartCalls = 0;
snd_pcm_sframes_t playbackAvailable = 0;
int closeResult = 0;
unsigned closeCalls = 0;

}

extern "C" int snd_pcm_recover(snd_pcm_t* pcm, int err, int silent) {
    recoveredPcm = pcm;
    recoveredError = err;
    recoverySilent = silent;
    ++recoveryCalls;
    return recoveryResult;
}

extern "C" snd_pcm_state_t snd_pcm_state(snd_pcm_t*) {
    return recoveryPcmState;
}

extern "C" int snd_pcm_start(snd_pcm_t*) {
    ++recoveryStartCalls;
    return recoveryStartResult;
}

extern "C" snd_pcm_sframes_t snd_pcm_avail_update(snd_pcm_t*) {
    return playbackAvailable;
}

extern "C" int snd_pcm_close(snd_pcm_t*) {
    ++closeCalls;
    return closeResult;
}

int main() {
    audiox::AdaptiveAudioBuffer adaptive(0);
    assert(adaptive.targetPeriodFrames(128) == 128);
    for (uint64_t t = 1000; t < 60000; t += 1000)
        assert(!adaptive.recordXrun(t, 128)); // Startup never triggers adaptation.
    adaptive.bufferChanged(2000); // Opening a device cannot shorten startup grace.
    assert(!adaptive.recordXrun(59999, 128));
    for (uint64_t t = 60000; t < 60004; ++t) assert(!adaptive.recordXrun(t, 128));
    assert(adaptive.level() == 0); // Four xruns are not enough to increase buffering.
    assert(adaptive.recordXrun(60004, 128)); // Startup events cannot count toward this threshold.
    assert(adaptive.level() == 1 && adaptive.targetPeriodFrames(128) == 256);
    assert(adaptive.increaseId() == 1 && adaptive.changeId() == 1);
    assert(!adaptive.recordXrun(70003, 128)); // An increase cannot cause a chain reaction.
    assert(adaptive.downshiftDelayMs() == 60000);
    assert(!adaptive.tick(130002)); // Settling xruns still postpone a downshift.
    assert(adaptive.tick(130003));
    assert(adaptive.level() == 0 && adaptive.targetPeriodFrames(128) == 128);
    assert(adaptive.changeId() == 2 && adaptive.increaseId() == 1);

    uint64_t retryAt = 131003;
    for (uint32_t failures = 1; failures <= 3; ++failures) {
        assert(!adaptive.recordXrun(retryAt - 1, 128));
        for (uint64_t t = retryAt; t < retryAt + 4; ++t)
            assert(!adaptive.recordXrun(t, 128));
        assert(adaptive.recordXrun(retryAt + 4, 128));
        assert(adaptive.downshiftFailures() == failures);
        const uint64_t delay = failures * 300000ULL;
        assert(adaptive.downshiftDelayMs() == delay);
        assert(!adaptive.tick(retryAt + 4 + delay - 1));
        assert(adaptive.tick(retryAt + 4 + delay));
        retryAt += 4 + delay + audiox::AdaptiveAudioBuffer::kDownshiftSettleMs;
    }

    audiox::AdaptiveAudioBuffer downshift(0);
    for (uint64_t t = 60000; t < 60004; ++t) assert(!downshift.recordXrun(t, 128));
    assert(downshift.recordXrun(60004, 128));
    assert(downshift.tick(120004));
    downshift.bufferChanged(120500); // Last stream finished reopening later.
    for (uint64_t t = 120500; t < 121500; t += 100)
        assert(!downshift.recordXrun(t, 128));
    for (uint64_t t = 121500; t < 121504; ++t) assert(!downshift.recordXrun(t, 128));
    assert(downshift.recordXrun(121504, 128));
    assert(downshift.level() == 1 && downshift.downshiftFailures() == 1);
    for (uint64_t t = 121505; t < 131504; t += 100)
        assert(!downshift.recordXrun(t, 128)); // An upshift retains its settling grace.
    assert(downshift.level() == 1);
    assert(!downshift.tick(421503)); // The most recent settling xrun resets quiet time.

    audiox::AdaptiveAudioBuffer expired(0);
    for (uint64_t t = 60000; t < 60004; ++t) assert(!expired.recordXrun(t, 128));
    assert(expired.recordXrun(60004, 128));
    assert(expired.tick(120004));
    expired.bufferChanged(720005); // An unrelated reopen after the retry window.
    for (uint64_t t = 721005; t < 721010; ++t) assert(!expired.recordXrun(t, 128));
    assert(expired.level() == 0 && expired.downshiftFailures() == 0);
    for (uint64_t t = 730005; t < 730009; ++t) assert(!expired.recordXrun(t, 128));
    assert(expired.recordXrun(730009, 128));
    assert(expired.downshiftFailures() == 0);

    audiox::AdaptiveAudioBuffer maxed(0);
    uint64_t eventTime = 60000;
    for (uint32_t expectedLevel = 1; expectedLevel <= 3; ++expectedLevel) {
        for (uint32_t i = 0; i < 4; ++i) assert(!maxed.recordXrun(eventTime + i * 100, 128));
        assert(maxed.recordXrun(eventTime + 400, 128));
        assert(maxed.level() == expectedLevel);
        eventTime += 11000;
    }
    assert(maxed.targetPeriodFrames(128) == audiox::AdaptiveAudioBuffer::kMaxPeriodFrames);
    eventTime += 1000;
    for (uint32_t i = 0; i < 5; ++i)
        assert(!maxed.recordXrun(eventTime + i * 100, 128));
    // The controller never exceeds 1024 frames.
    assert(maxed.level() == 3 && maxed.increaseId() == 3);

    audiox::AdaptiveAudioBuffer windowed(0);
    for (uint64_t t = 60000; t < 60004; ++t) assert(!windowed.recordXrun(t, 128));
    assert(!windowed.recordXrun(90004, 128)); // Old events outside 30 seconds do not escalate.
    assert(windowed.level() == 0);

    audiox::AdaptiveAudioBuffer settling(0);
    for (uint64_t t = 60000; t < 60004; ++t) assert(!settling.recordXrun(t, 128));
    assert(settling.recordXrun(60004, 128));
    settling.bufferChanged(65000); // Grace runs from completion, not just the decision to reopen.
    for (uint64_t t = 70004; t < 75000; ++t) assert(!settling.recordXrun(t, 128));
    assert(!settling.tick(74999));
    for (uint64_t t = 75000; t < 75004; ++t) assert(!settling.recordXrun(t, 128));
    assert(settling.recordXrun(75004, 128));
    assert(settling.level() == 2 && settling.downshiftFailures() == 0);

    audiox::AudioHealth health(0);
    health.devicesChanged(1000);
    health.bufferChanged(2000); // Neither grace extension may shorten the startup window.
    for (uint64_t t : {55000, 56000, 57000, 58000, 59999}) assert(!health.recordXrun(t));
    assert(health.warningAt() == 0 && health.total() == 5);
    for (uint64_t t : {60000, 61000, 62000, 63000}) assert(!health.recordXrun(t));
    assert(health.recordXrun(64000));
    assert(health.warningActive(94000) && !health.warningActive(94001));
    assert(!health.recordXrun(95000));
    assert(health.warningActive(125000) && !health.warningActive(125001));
    for (uint64_t t = 96000; t < 184000; t += 1000) assert(!health.recordXrun(t));
    assert(health.recordXrun(184000));
    health.bufferChanged(185000);
    assert(!health.warningActive(185000));
    for (uint64_t t = 185000; t < 195000; t += 1000) assert(!health.recordXrun(t));
    for (uint64_t t : {304000, 315000, 325000, 335000}) assert(!health.recordXrun(t));
    assert(!health.recordXrun(344001));
    assert(health.recordXrun(344002));
    if (audio_pcm_recover(nullptr, -EPIPE, "test", "playback") != -EINVAL ||
        recoveryCalls != 0) {
        std::fprintf(stderr, "null PCM handle reached ALSA recovery\n");
        return 1;
    }

    snd_pcm_t pcm = {};
    assert(audio_pcm_close(&pcm, "test", "playback") == 0);
    closeResult = -EBADFD;
    assert(audio_pcm_close(&pcm, "test", "playback") == -EBADFD);
    assert(closeCalls == 2);
    assert(audio_pcm_playback_start_frames(128, 512) == 128);
    assert(audio_pcm_playback_start_frames(1024, 4096) == 3072);
    assert(audio_pcm_playback_start_frames(1024, 1024) == 1024);
    for (snd_pcm_uframes_t period : {128U, 256U, 512U, 1024U}) {
        const auto buffer = period * 4U;
        const auto target = audio_pcm_playback_start_frames(period, buffer);
        const unsigned starts = recoveryStartCalls;
        playbackAvailable = buffer - target + 1;
        assert(audio_pcm_start_playback_if_ready(&pcm, period, buffer, "test") == 0);
        assert(recoveryStartCalls == starts); // One frame short: do not start yet.
        playbackAvailable = buffer - target;
        assert(audio_pcm_start_playback_if_ready(&pcm, period, buffer, "test") == 0);
        assert(recoveryStartCalls == starts + 1);
    }
    recoveryStartCalls = 0;
    assert(audio_pcm_recover_capture(&pcm, -EPIPE, "capture-test") == 0);
    assert(recoveryStartCalls == 1); // Recovering an xrun prepares capture; restart it.
    recoveryPcmState = SND_PCM_STATE_RUNNING;
    assert(audio_pcm_recover_capture(&pcm, -EINTR, "capture-test") == 0);
    assert(recoveryStartCalls == 1);
    recoveryPcmState = SND_PCM_STATE_PREPARED;
    recoveryStartResult = -EIO;
    assert(audio_pcm_recover_capture(&pcm, -EPIPE, "capture-test") == -EIO);
    assert(recoveryStartCalls == 2);
    recoveryStartResult = 0;
    recoveryCalls = 0;

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
    assert(audiox::audioHealth.total() == 4); // ESTRPIPE and EINTR are not X-runs.

    std::puts("audio PCM recovery tests passed");
    return 0;
}