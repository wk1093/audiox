#include "audio/graph_native.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;
namespace audiox::graph {
namespace {
using Clock = std::chrono::steady_clock;
bool transfer(int fd, void *data, size_t size, bool writing, Clock::time_point deadline) {
    auto *bytes = static_cast<unsigned char *>(data);
    while (size) {
        pollfd descriptor{fd, static_cast<short>(writing ? POLLOUT : POLLIN), 0};
        int ready;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return false;
        }
        ready = poll(&descriptor, 1, static_cast<int>(remaining));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0) {
            if (!ready) errno = ETIMEDOUT;
            return false;
        }
        const ssize_t count = writing ? send(fd, bytes, size, MSG_NOSIGNAL)
                                      : recv(fd, bytes, size, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) {
            if (!count) errno = EPIPE;
            return false;
        }
        bytes += count;
        size -= static_cast<size_t>(count);
    }
    return true;
}
}

NativeGraph::NativeGraph(const char *compiler) : compiler_(compiler) {
    running_.store(true);
    const int result = pthread_create(&thread_, nullptr, threadMain, this);
    if (result) {
        running_.store(false);
        std::fprintf(stderr, "[GRAPH] [ERR] compiler thread: %s; using existing executor\n",
                     std::strerror(result));
    }
}
NativeGraph::~NativeGraph() {
    if (running_.exchange(false)) pthread_join(thread_, nullptr);
    for (auto &program : programs_) clear(program);
}
void NativeGraph::clear(Program &program) {
    if (program.memory && munmap(program.memory, program.size))
        std::fprintf(stderr, "[GRAPH] [ERR] release native code: %s\n", std::strerror(errno));
    program = {};
}
void NativeGraph::request(const Schedule &schedule) {
    requests_[requestBack_] = schedule;
    requestBack_ = requestMiddle_.exchange(requestBack_ | 4U, std::memory_order_acq_rel) & 3U;
}
bool NativeGraph::execute(uint32_t generation, void *context, Callback route, Callback node) {
    if (programMiddle_.load(std::memory_order_acquire) & 4U)
        programFront_ = programMiddle_.exchange(programFront_, std::memory_order_acq_rel) & 3U;
    const auto &program = programs_[programFront_];
    if (!program.memory || program.generation != generation) return false;
    reinterpret_cast<NativeEntry>(program.memory)(context, route, node);
    return true;
}
void *NativeGraph::threadMain(void *opaque) {
    auto &self = *static_cast<NativeGraph *>(opaque);
    while (self.running_.load(std::memory_order_acquire)) {
        if (self.requestMiddle_.load(std::memory_order_acquire) & 4U) {
            self.requestFront_ = self.requestMiddle_.exchange(self.requestFront_,
                std::memory_order_acq_rel) & 3U;
            self.compile(self.requests_[self.requestFront_]);
        } else {
            timespec delay{0, 10000000};
            nanosleep(&delay, nullptr);
        }
    }
    return nullptr;
}
void NativeGraph::compile(const Schedule &schedule) {
    const auto started = Clock::now();
    if (!schedule.generation || schedule.count > kMaxOperations) {
        std::fprintf(stderr, "[GRAPH] [ERR] invalid schedule; using existing executor\n");
        return;
    }
    int sockets[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets)) {
        std::fprintf(stderr, "[GRAPH] [ERR] compiler socket: %s\n", std::strerror(errno));
        return;
    }
    for (auto &socket : sockets) {
        if (socket <= STDERR_FILENO) {
            const int moved = fcntl(socket, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
            if (moved < 0) {
                const int error = errno;
                close(sockets[0]);
                close(sockets[1]);
                std::fprintf(stderr, "[GRAPH] [ERR] compiler descriptors: %s\n", std::strerror(error));
                return;
            }
            close(socket);
            socket = moved;
        }
    }
    posix_spawn_file_actions_t actions;
    int result = posix_spawn_file_actions_init(&actions);
    if (!result) {
        result = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDIN_FILENO);
        if (!result) result = posix_spawn_file_actions_adddup2(&actions, sockets[1], STDOUT_FILENO);
        if (!result) result = posix_spawn_file_actions_addclose(&actions, sockets[0]);
        pid_t child{};
        char *arguments[]{const_cast<char *>(compiler_), nullptr};
        if (!result) result = posix_spawn(&child, compiler_, &actions, nullptr, arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (!result) {
            NativeHeader header{};
            Schedule request = schedule;
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            bool ok = transfer(sockets[0], &request, sizeof(request), true, deadline) &&
                      transfer(sockets[0], &header, sizeof(header), false, deadline);
            Program candidate{};
            if (ok && (header.magic != kNativeMagic || !header.size || header.size > kMaxNativeBytes)) {
                errno = EINVAL;
                ok = false;
            }
            if (ok) {
                candidate.size = header.size;
                candidate.generation = schedule.generation;
                candidate.memory = mmap(nullptr, header.size, PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
                if (candidate.memory == MAP_FAILED) {
                    candidate.memory = nullptr;
                    ok = false;
                } else {
                    ok = transfer(sockets[0], candidate.memory, header.size, false, deadline);
                }
            }
            if (ok) {
                __builtin___clear_cache(static_cast<char *>(candidate.memory),
                                        static_cast<char *>(candidate.memory) + candidate.size);
                ok = mprotect(candidate.memory, candidate.size, PROT_READ | PROT_EXEC) == 0;
            }
            int error = errno;
            close(sockets[0]);
            int status{};
            bool reaped = false;
            while (ok && !reaped) {
                const pid_t waited = waitpid(child, &status, WNOHANG);
                if (waited == child) {
                    reaped = true;
                    if (!WIFEXITED(status) || WEXITSTATUS(status)) {
                        error = EIO;
                        ok = false;
                        std::fprintf(stderr, "[GRAPH] [ERR] compiler exit status %d\n", status);
                    }
                } else if (waited < 0 && errno != EINTR) {
                    error = errno;
                    ok = false;
                } else if (Clock::now() >= deadline) {
                    error = ETIMEDOUT;
                    ok = false;
                } else {
                    timespec delay{0, 1000000};
                    nanosleep(&delay, nullptr);
                }
            }
            if (!reaped) {
                if (kill(child, SIGKILL) && errno != ESRCH)
                    std::fprintf(stderr, "[GRAPH] [ERR] stop compiler: %s\n", std::strerror(errno));
                pid_t waited;
                do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
                if (waited < 0)
                    std::fprintf(stderr, "[GRAPH] [ERR] reap compiler: %s\n", std::strerror(errno));
            }
            if (!ok) {
                clear(candidate);
                std::fprintf(stderr, "[GRAPH] [ERR] compile generation %u: %s; using existing executor\n",
                             schedule.generation, std::strerror(error));
                return;
            }
            clear(programs_[programBack_]);
            programs_[programBack_] = candidate;
            programBack_ = programMiddle_.exchange(programBack_ | 4U,
                                                   std::memory_order_acq_rel) & 3U;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - started).count();
            std::printf("[GRAPH] [INFO] LLVM compiled generation %u: %u operations, %u bytes in %lld us\n",
                        schedule.generation, schedule.count, candidate.size,
                        static_cast<long long>(elapsed));
            return;
        }
    } else {
        close(sockets[1]);
    }
    close(sockets[0]);
    std::fprintf(stderr, "[GRAPH] [ERR] launch compiler: %s; using existing executor\n",
                 std::strerror(result));
}
} // namespace audiox::graph
