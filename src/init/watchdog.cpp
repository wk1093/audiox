#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

namespace {

constexpr int kHeartbeatFd = 3;
#ifndef AUDIOX_WATCHDOG_STARTUP_TIMEOUT_MS
#define AUDIOX_WATCHDOG_STARTUP_TIMEOUT_MS 120000
#endif
#ifndef AUDIOX_WATCHDOG_HEARTBEAT_TIMEOUT_MS
#define AUDIOX_WATCHDOG_HEARTBEAT_TIMEOUT_MS 12000
#endif
constexpr uint64_t kStartupTimeoutMs = AUDIOX_WATCHDOG_STARTUP_TIMEOUT_MS;
constexpr uint64_t kHeartbeatTimeoutMs = AUDIOX_WATCHDOG_HEARTBEAT_TIMEOUT_MS;
constexpr unsigned kMaximumRestartDelaySeconds = 30;

uint64_t monotonicMilliseconds() {
    timespec now = {};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
        return 0;
    }
    return static_cast<uint64_t>(now.tv_sec) * 1000U +
        static_cast<uint64_t>(now.tv_nsec) / 1000000U;
}

void sleepSeconds(unsigned seconds) {
    timespec remaining = {static_cast<time_t>(seconds), 0};
    while (nanosleep(&remaining, &remaining) < 0 && errno == EINTR) {
    }
}

pid_t startRuntime(const char* runtimePath, int* heartbeatReadFd) {
    int heartbeatPipe[2];
    if (pipe(heartbeatPipe) < 0) {
        perror("[WATCHDOG] pipe");
        return -1;
    }

    const pid_t child = fork();
    if (child < 0) {
        perror("[WATCHDOG] fork");
        close(heartbeatPipe[0]);
        close(heartbeatPipe[1]);
        return -1;
    }

    if (child == 0) {
        close(heartbeatPipe[0]);
        if (setpgid(0, 0) < 0) {
            perror("[WATCHDOG] setpgid child");
            _exit(126);
        }
        if (heartbeatPipe[1] != kHeartbeatFd) {
            if (dup2(heartbeatPipe[1], kHeartbeatFd) < 0) {
                perror("[WATCHDOG] dup2 heartbeat");
                _exit(126);
            }
            close(heartbeatPipe[1]);
        }
        const int flags = fcntl(kHeartbeatFd, F_GETFL, 0);
        if (flags >= 0) {
            (void)fcntl(kHeartbeatFd, F_SETFL, flags | O_NONBLOCK);
        }
        (void)setenv("AUDIOX_WATCHDOG_FD", "3", 1);
        execl(runtimePath, runtimePath, static_cast<char*>(nullptr));
        dprintf(STDERR_FILENO, "[WATCHDOG] exec %s failed: %s\n", runtimePath, strerror(errno));
        _exit(127);
    }

    close(heartbeatPipe[1]);
    (void)setpgid(child, child);
    const int flags = fcntl(heartbeatPipe[0], F_GETFL, 0);
    if (flags >= 0) {
        (void)fcntl(heartbeatPipe[0], F_SETFL, flags | O_NONBLOCK);
    }
    *heartbeatReadFd = heartbeatPipe[0];
    return child;
}

void drainHeartbeats(int fd, uint64_t* lastSequence, uint64_t* lastProgressMs) {
    uint64_t sequence = 0;
    while (read(fd, &sequence, sizeof(sequence)) == static_cast<ssize_t>(sizeof(sequence))) {
        if (sequence != *lastSequence) {
            *lastSequence = sequence;
            *lastProgressMs = monotonicMilliseconds();
        }
    }
}

bool waitForRuntime(pid_t child, int heartbeatFd, uint64_t launchTime, const char** reason) {
    uint64_t lastSequence = 0;
    uint64_t lastProgressTime = launchTime;
    bool receivedHeartbeat = false;

    for (;;) {
        pollfd heartbeat = {heartbeatFd, POLLIN, 0};
        const int pollResult = poll(&heartbeat, 1, 500);
        if (pollResult > 0 && (heartbeat.revents & POLLIN)) {
            const uint64_t previousSequence = lastSequence;
            drainHeartbeats(heartbeatFd, &lastSequence, &lastProgressTime);
            receivedHeartbeat = receivedHeartbeat || lastSequence != previousSequence;
        } else if (pollResult < 0 && errno != EINTR) {
            perror("[WATCHDOG] poll");
        }

        int status = 0;
        const pid_t result = waitpid(child, &status, WNOHANG);
        if (result == child) {
            *reason = WIFEXITED(status) ? "main process exited" : "main process terminated by signal";
            return true;
        }
        if (result < 0 && errno != EINTR) {
            perror("[WATCHDOG] waitpid");
            *reason = "waitpid failed";
            return true;
        }

        const uint64_t now = monotonicMilliseconds();
        if (receivedHeartbeat && now - lastProgressTime > kHeartbeatTimeoutMs) {
            *reason = "main-loop heartbeat timed out";
            return false;
        }
        if (!receivedHeartbeat && now - launchTime > kStartupTimeoutMs) {
            *reason = "main process startup timed out before first heartbeat";
            return false;
        }
    }
}

}  // namespace

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* runtimePath = argc > 1 ? argv[1] : "/sbin/audiox-main";
    const unsigned maximumRestarts = argc > 2 ? static_cast<unsigned>(strtoul(argv[2], nullptr, 10)) : 0U;
    printf("[WATCHDOG] PID 1 supervisor pid=%d runtime=%s\n", static_cast<int>(getpid()), runtimePath);
    unsigned restartDelaySeconds = 1;
    unsigned restartCount = 0;

    for (;;) {
        int heartbeatFd = -1;
        const uint64_t launchTime = monotonicMilliseconds();
        const pid_t child = startRuntime(runtimePath, &heartbeatFd);
        if (child < 0) {
            sleepSeconds(restartDelaySeconds);
            if (restartDelaySeconds < kMaximumRestartDelaySeconds) {
                restartDelaySeconds *= 2;
                if (restartDelaySeconds > kMaximumRestartDelaySeconds) {
                    restartDelaySeconds = kMaximumRestartDelaySeconds;
                }
            }
            continue;
        }

        printf("[WATCHDOG] started main process pid=%d\n", static_cast<int>(child));
        const char* reason = "unknown failure";
        const bool childExited = waitForRuntime(child, heartbeatFd, launchTime, &reason);
        if (!childExited) {
            printf("[WATCHDOG] %s; killing pid=%d\n", reason, static_cast<int>(child));
            (void)kill(-child, SIGKILL);
            int status = 0;
            while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
            }
        } else {
            printf("[WATCHDOG] %s (pid=%d); restarting\n", reason, static_cast<int>(child));
            (void)kill(-child, SIGKILL);
        }
        int orphanStatus = 0;
        while (waitpid(-1, &orphanStatus, WNOHANG) > 0) {
        }
        close(heartbeatFd);
        ++restartCount;
        if (maximumRestarts != 0 && restartCount >= maximumRestarts) {
            return 0;
        }

        const uint64_t runTime = monotonicMilliseconds() - launchTime;
        if (runTime >= 60000U) {
            restartDelaySeconds = 1;
        } else if (restartDelaySeconds < kMaximumRestartDelaySeconds) {
            restartDelaySeconds *= 2;
            if (restartDelaySeconds > kMaximumRestartDelaySeconds) {
                restartDelaySeconds = kMaximumRestartDelaySeconds;
            }
        }
        printf("[WATCHDOG] next start in %u second(s)\n", restartDelaySeconds);
        sleepSeconds(restartDelaySeconds);
    }
}
