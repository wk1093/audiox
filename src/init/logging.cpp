#include "init.hpp"
#include "context.hpp"

int setupLogging(void *_context) {
    Audiox *context = (Audiox *)_context;
    if (!context) {
        return RET_ERR;
    }
    // TODO
    return RET_WARN;
}