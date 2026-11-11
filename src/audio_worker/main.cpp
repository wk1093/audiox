#include "audio/worker.hpp"
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>

int main(int argc, char **argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    if (argc != 3) {
        fprintf(stderr, "usage: audiox-audio-worker SHARED_FD PARENT_PID\n");
        return 2;
    }
    char *end = nullptr;
    errno = 0;
    const long fd = strtol(argv[1], &end, 10);
    if (errno || !end || *end || fd < 0 || fd > INT_MAX) return 2;
    const long parent = strtol(argv[2], &end, 10);
    if (errno || !end || *end || parent <= 1 || parent > INT_MAX) return 2;
    return audiox::worker::run(static_cast<int>(fd), static_cast<int>(parent));
}
