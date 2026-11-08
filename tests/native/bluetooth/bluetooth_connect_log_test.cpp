#include "bluetooth/context.hpp"
#include "audio/context.hpp"

#include <cassert>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {
int connectError = EHOSTDOWN;

std::string capture(BluetoothContext &context, bool automatic, bool pending, bool connected = false,
                    const char *reason = "Host is down", int immediateError = EHOSTDOWN) {
    FILE *output = tmpfile();
    assert(output);
    fflush(stdout);
    const int saved = dup(STDOUT_FILENO);
    assert(saved >= 0 && dup2(fileno(output), STDOUT_FILENO) >= 0);

    connectError = pending ? EINPROGRESS : immediateError;
    const bt::Address target = context.devices[0].address;
    assert(context.beginConnect(target, automatic) == pending);
    if (pending) {
        assert(context.links[0].used && context.links[0].automaticConnect == automatic);
        if (connected) {
            context.links[0].signalingConnecting = false;
            context.closeLink(0, reason);
        } else if (strcmp(reason, "Host is down") == 0) {
            context.handleSignalingConnected(0);
        } else if (strcmp(reason, "A2DP connection timed out") == 0) {
            context.serviceLinkTimers(0, context.links[0].deadlineMs);
        } else {
            context.closeLink(0, reason);
        }
        assert(!context.links[0].used);
    }

    fflush(stdout);
    assert(dup2(saved, STDOUT_FILENO) >= 0);
    close(saved);
    rewind(output);
    std::string text;
    char buffer[256];
    while (fgets(buffer, sizeof(buffer), output)) text += buffer;
    fclose(output);
    return text;
}
}

extern "C" int __wrap_socket(int, int, int) {
    return dup(STDIN_FILENO);
}

extern "C" int __wrap_setsockopt(int, int, int, const void *, socklen_t) {
    return 0;
}

extern "C" int __wrap_connect(int, const struct sockaddr *, socklen_t) {
    errno = connectError;
    return -1;
}

extern "C" int __wrap_getsockopt(int, int, int, void *value, socklen_t *length) {
    assert(*length >= sizeof(int));
    *static_cast<int *>(value) = EHOSTDOWN;
    *length = sizeof(int);
    return 0;
}

int AudioContext::reloadRoutingGraph() { return RET_OK; }

int main() {
    BluetoothContext context(nullptr);
    context.deviceCount = 1;
    context.devices[0].address.b[0] = 1;
    context.devices[0].paired = true;
    snprintf(context.devices[0].name, sizeof(context.devices[0].name), "Speaker");

    assert(capture(context, true, false).empty());
    assert(capture(context, true, false, false, "Host is down", ENETDOWN).empty());
    assert(capture(context, false, false).find("signaling connect failed") != std::string::npos);
    assert(capture(context, true, true).empty());
    assert(strstr(context.message, "Host is down"));
    assert(context.devices[0].autoConnectAtMs > 0);
    assert(capture(context, true, true, false, "A2DP connection timed out").empty());
    assert(capture(context, false, true).find("disconnected: Host is down") != std::string::npos);
    assert(capture(context, true, true, true).find("disconnected: Host is down") != std::string::npos);
    assert(capture(context, true, true, false, "disconnected by user").find("disconnected by user") != std::string::npos);
    puts("Bluetooth auto-connect logging tests passed");
}
