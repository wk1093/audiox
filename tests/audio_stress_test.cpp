#include "audio/stress_test.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>

int main() {
    audiox::AudioStressTest stress;
    assert(stress.mode() == audiox::AudioStressTest::Idle);
    assert(stress.startAudioThread(31, 1) == -EINVAL);
    assert(stress.startAudioThread(1, 21) == -EINVAL);
    assert(stress.startCpuContention(0) == -EINVAL);

    assert(stress.startAudioThread(10, 1) == 0);
    assert(stress.mode() == audiox::AudioStressTest::AudioThread);
    assert(stress.remainingMs() > 0);
    assert(stress.startCpuContention(1) == -EBUSY);
    stress.processAudioBlock();
    stress.stop();
    assert(stress.mode() == audiox::AudioStressTest::Idle);

    assert(stress.startCpuContention(10) == 0);
    assert(stress.mode() == audiox::AudioStressTest::CpuContention);
    assert(stress.workerCount() > 0 && stress.workerCount() <= audiox::AudioStressTest::kMaxWorkers);
    assert(stress.startAudioThread(1, 1) == -EBUSY);
    stress.stop();
    assert(stress.mode() == audiox::AudioStressTest::Idle);
    assert(stress.workerCount() == 0);

    std::puts("bounded audio stress-test controls passed");
    return 0;
}
