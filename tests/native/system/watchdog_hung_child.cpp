#include <cstdint>
#include <cstdlib>
#include <unistd.h>

int main() {
    const char* descriptorText = std::getenv("AUDIOX_WATCHDOG_FD");
    if (!descriptorText) {
        return 2;
    }
    const int descriptor = std::atoi(descriptorText);
    const uint64_t firstHeartbeat = 1;
    if (write(descriptor, &firstHeartbeat, sizeof(firstHeartbeat)) !=
        static_cast<ssize_t>(sizeof(firstHeartbeat))) {
        return 3;
    }
    for (;;) {
        pause();
    }
}
