#include "init.hpp"
#include "context.hpp"

int setupAudioGadget(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }
    // TODO
    return RET_WARN;
}

int setupNetworkGadget(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }
    // TODO
    return RET_WARN;
}

int bindUsbGadget(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }
    // TODO
    return RET_WARN;
}