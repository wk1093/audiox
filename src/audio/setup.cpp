#include "audio/context.hpp"

AudioContext::AudioContext(Audiox *context) : app(context) {
}

int AudioContext::setupThreads() {
    // TODO
    return RET_WARN;
}

void AudioContext::poll() {
    // TODO
}
