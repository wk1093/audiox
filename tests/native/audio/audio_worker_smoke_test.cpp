#include "audio/worker_protocol.hpp"
#include "audio/health.hpp"
#include <cassert>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <linux/memfd.h>
#include <new>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

int main(int argc, char **argv) {
    assert(argc == 3);
    using namespace audiox::worker;
    const int fd = syscall(SYS_memfd_create, "audio-worker-smoke", MFD_CLOEXEC);
    assert(fd >= 0 && ftruncate(fd, sizeof(Shared)) == 0);
    auto *memory = mmap(nullptr, sizeof(Shared), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    assert(memory != MAP_FAILED);
    auto &shared = *new (memory) Shared;
    Control control;
    control.effectSequence = 1;
    shared.control.publish(control);
    posix_spawn_file_actions_t actions;
    assert(posix_spawn_file_actions_init(&actions) == 0);
    assert(posix_spawn_file_actions_adddup2(&actions, fd, 3) == 0);
    char parent[24];
    snprintf(parent, sizeof(parent), "%d", getpid());
    char *args[] = {argv[1], argv[2], const_cast<char *>("3"), parent, nullptr};
    pid_t pid;
    assert(posix_spawnp(&pid, argv[1], &actions, nullptr, args, environ) == 0);
    posix_spawn_file_actions_destroy(&actions);
    const auto deadline = audiox::healthMonotonicMs() + 15000;
    bool responsive = false;
    Telemetry telemetry;
    while (audiox::healthMonotonicMs() < deadline) {
        int status = 0;
        const auto exited = waitpid(pid, &status, WNOHANG);
        assert(exited == 0);
        if (shared.ready.load(std::memory_order_acquire) &&
            shared.heartbeat.load(std::memory_order_acquire) &&
            shared.telemetry.consume(telemetry)) {
            responsive = true;
            break;
        }
        usleep(10000);
    }
    shared.stop.store(true, std::memory_order_release);
    int status = 0;
    bool stopped = false;
    for (unsigned i = 0; i < 500; ++i) {
        if (waitpid(pid, &status, WNOHANG) == pid) { stopped = true; break; }
        usleep(10000);
    }
    if (!stopped) {
        assert(kill(pid, SIGKILL) == 0);
        assert(waitpid(pid, &status, 0) == pid);
    }
    assert(responsive && stopped && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(telemetry.bufferRuntime != 0 && telemetry.activeStreams == 0);
    munmap(memory, sizeof(Shared));
    close(fd);
    puts("Production ARM audio worker: protocol, configuration, idle audio heartbeat, telemetry and graceful shutdown passed");
}
