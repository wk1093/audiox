#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "boot_recovery.hpp"

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

unsigned nextBackoff(unsigned delaySeconds) {
    if (delaySeconds >= kMaximumRestartDelaySeconds) {
        return kMaximumRestartDelaySeconds;
    }
    delaySeconds *= 2;
    return delaySeconds > kMaximumRestartDelaySeconds ? kMaximumRestartDelaySeconds : delaySeconds;
}

// Long-lived helper daemon (dnsmasq) supervised independently of the runtime.
// It runs in its own process group so runtime-group kills never reach it.
struct Service {
    const char* name = "dnsmasq";
    const char* path = nullptr;
    const char* configPath = nullptr;
    bool enabled = false;
    pid_t pid = -1;
    uint64_t startedMs = 0;
    uint64_t nextStartMs = 0;
    unsigned restartDelaySeconds = 1;
};

void startService(Service* service, uint64_t now) {
    char configArgument[512];
    snprintf(configArgument, sizeof(configArgument), "--conf-file=%s", service->configPath);

    const pid_t child = fork();
    if (child < 0) {
        perror("[WATCHDOG] fork service");
        service->nextStartMs = now + service->restartDelaySeconds * 1000U;
        service->restartDelaySeconds = nextBackoff(service->restartDelaySeconds);
        return;
    }
    if (child == 0) {
        (void)setpgid(0, 0);
        execl(service->path, service->path, "--keep-in-foreground", configArgument,
              static_cast<char*>(nullptr));
        dprintf(STDERR_FILENO, "[WATCHDOG] exec %s failed: %s\n", service->path, strerror(errno));
        _exit(127);
    }
    (void)setpgid(child, child);
    service->pid = child;
    service->startedMs = now;
    printf("[WATCHDOG] started %s pid=%d\n", service->name, static_cast<int>(child));
}

void handleServiceExit(Service* service, int status, uint64_t now) {
    if (WIFEXITED(status)) {
        printf("[WATCHDOG] %s pid=%d exited with status %d\n", service->name,
               static_cast<int>(service->pid), WEXITSTATUS(status));
    } else {
        printf("[WATCHDOG] %s pid=%d terminated by signal %d\n", service->name,
               static_cast<int>(service->pid), WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    }
    service->pid = -1;
    if (now - service->startedMs >= 60000U) {
        service->restartDelaySeconds = 1;
    }
    service->nextStartMs = now + service->restartDelaySeconds * 1000U;
    printf("[WATCHDOG] restarting %s in %u second(s)\n", service->name, service->restartDelaySeconds);
    service->restartDelaySeconds = nextBackoff(service->restartDelaySeconds);
}

void superviseService(Service* service) {
    if (!service || !service->enabled) {
        return;
    }
    const uint64_t now = monotonicMilliseconds();
    if (service->pid > 0) {
        int status = 0;
        const pid_t result = waitpid(service->pid, &status, WNOHANG);
        if (result == service->pid) {
            handleServiceExit(service, status, now);
        } else if (result < 0 && errno == ECHILD) {
            service->pid = -1;
            service->nextStartMs = now;
        }
        return;
    }
    if (now >= service->nextStartMs) {
        startService(service, now);
    }
}

void reapOrphans(Service* service) {
    int status = 0;
    pid_t reaped = 0;
    while ((reaped = waitpid(-1, &status, WNOHANG)) > 0) {
        if (service && reaped == service->pid) {
            handleServiceExit(service, status, monotonicMilliseconds());
        }
    }
}

void sleepSupervising(unsigned seconds, Service* service) {
    const uint64_t deadline = monotonicMilliseconds() + seconds * 1000U;
    for (;;) {
        superviseService(service);
        const uint64_t now = monotonicMilliseconds();
        if (now >= deadline) {
            return;
        }
        const uint64_t remainingMs = deadline - now;
        const uint64_t stepMs = remainingMs < 500U ? remainingMs : 500U;
        timespec step = {static_cast<time_t>(stepMs / 1000U), static_cast<long>((stepMs % 1000U) * 1000000U)};
        while (nanosleep(&step, &step) < 0 && errno == EINTR) {
        }
    }
}

void stopService(Service* service) {
    if (!service || service->pid <= 0) {
        return;
    }
    (void)kill(-service->pid, SIGTERM);
    int status = 0;
    while (waitpid(service->pid, &status, 0) < 0 && errno == EINTR) {
    }
    service->pid = -1;
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
    (void)fcntl(heartbeatPipe[0], F_SETFD, FD_CLOEXEC);
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

bool waitForRuntime(pid_t child, int heartbeatFd, uint64_t launchTime, Service* service, const char** reason) {
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
        superviseService(service);

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

bool bootTrialStillPending() {
    const char* trial = getenv(BOOT_RECOVERY_TRIAL_ENV);
    if (!trial || strcmp(trial, "1") != 0) {
        return false;
    }
    if (access(BOOT_RECOVERY_TRIAL_PATH, F_OK) == 0) {
        return true;
    }
    // Without the config partition mounted the runtime cannot have confirmed the trial.
    struct stat rootStat = {};
    struct stat configStat = {};
    if (stat("/", &rootStat) < 0 || stat(ROOT_MOUNT_POINT, &configStat) < 0) {
        return true;
    }
    return rootStat.st_dev == configStat.st_dev;
}

void rebootFailedTrial(const char* reason) {
    printf("[WATCHDOG] trial boot failed: %s; rebooting\n", reason);
    struct stat configStat = {};
    if (stat(BOOT_RECOVERY_STATE_DIR, &configStat) == 0) {
        char line[256];
        snprintf(line, sizeof(line), "[WATCHDOG] trial boot failed: %s\n", reason);
        brAppendText(BOOT_RECOVERY_EVENTS_PATH, line);
    }
    sync();
    if (reboot(RB_AUTOBOOT) < 0) {
        perror("[WATCHDOG] reboot");
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

    // An empty service path disables it (used by host tests).
    Service dnsmasq;
    dnsmasq.path = argc > 3 ? argv[3] : "/sbin/dnsmasq";
    dnsmasq.configPath = argc > 4 ? argv[4] : "/etc/dnsmasq.conf";
    if (dnsmasq.path[0] == '\0') {
        printf("[WATCHDOG] dnsmasq disabled\n");
    } else if (access(dnsmasq.path, X_OK) != 0 || access(dnsmasq.configPath, R_OK) != 0) {
        printf("[WATCHDOG] dnsmasq unavailable (%s, %s); USB DHCP/DNS disabled\n",
               dnsmasq.path, dnsmasq.configPath);
    } else {
        dnsmasq.enabled = true;
        superviseService(&dnsmasq);
    }

    for (;;) {
        int heartbeatFd = -1;
        const uint64_t launchTime = monotonicMilliseconds();
        const pid_t child = startRuntime(runtimePath, &heartbeatFd);
        if (child < 0) {
            sleepSupervising(restartDelaySeconds, &dnsmasq);
            restartDelaySeconds = nextBackoff(restartDelaySeconds);
            continue;
        }

        printf("[WATCHDOG] started main process pid=%d\n", static_cast<int>(child));
        const char* reason = "unknown failure";
        const bool childExited = waitForRuntime(child, heartbeatFd, launchTime, &dnsmasq, &reason);
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
        reapOrphans(&dnsmasq);
        close(heartbeatFd);
        if (bootTrialStillPending()) {
            rebootFailedTrial(reason);
        }
        ++restartCount;
        if (maximumRestarts != 0 && restartCount >= maximumRestarts) {
            stopService(&dnsmasq);
            return 0;
        }

        const uint64_t runTime = monotonicMilliseconds() - launchTime;
        if (runTime >= 60000U) {
            restartDelaySeconds = 1;
        } else {
            restartDelaySeconds = nextBackoff(restartDelaySeconds);
        }
        printf("[WATCHDOG] next start in %u second(s)\n", restartDelaySeconds);
        sleepSupervising(restartDelaySeconds, &dnsmasq);
    }
}
