#include "audio/graph_native.hpp"

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <cmath>
#include <cstdlib>
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

NativeGraph::NativeGraph(const char *compiler, const char *bitcode) : compiler_(compiler), bitcode_(bitcode) {
    if (sem_init(&wake_, 0, 0)) {
        std::fprintf(stderr, "[GRAPH] [ERR] compiler wakeup: %s; built-in DSP muted\n", std::strerror(errno));
        return;
    }
    wakeInitialized_ = true;
    running_.store(true);
    const int result = pthread_create(&thread_, nullptr, threadMain, this);
    if (result) {
        running_.store(false);
        std::fprintf(stderr, "[GRAPH] [ERR] compiler thread: %s; built-in DSP muted\n",
                     std::strerror(result));
    }
}
NativeGraph::~NativeGraph() {
    if (running_.exchange(false)) {
        if (sem_post(&wake_))
            std::fprintf(stderr, "[GRAPH] [ERR] stop compiler wakeup: %s\n", std::strerror(errno));
        pthread_join(thread_, nullptr);
    }
    if (wakeInitialized_ && sem_destroy(&wake_))
        std::fprintf(stderr, "[GRAPH] [ERR] release compiler wakeup: %s\n", std::strerror(errno));
    for (auto &program : programs_) clear(program);
}
void NativeGraph::clear(Program &program) {
    if (program.memory && munmap(program.memory, program.size))
        std::fprintf(stderr, "[GRAPH] [ERR] release native code: %s\n", std::strerror(errno));
    program = {};
}
void NativeGraph::request(const Schedule &schedule) {
    requests_[requestBack_] = schedule;
    const unsigned previous = requestMiddle_.exchange(requestBack_ | 4U, std::memory_order_acq_rel);
    requestBack_ = previous & 3U;
    if (!(previous & 4U) && wakeInitialized_ && sem_post(&wake_))
        std::fprintf(stderr, "[GRAPH] [ERR] wake compiler: %s\n", std::strerror(errno));
}
bool NativeGraph::execute(uint32_t generation, void *context, Callback route, Callback node,
                         void (*activate)(const audiox::effects::DspTable *)) {
    if (programMiddle_.load(std::memory_order_acquire) & 4U)
        programFront_ = programMiddle_.exchange(programFront_, std::memory_order_acq_rel) & 3U;
    const auto &program = programs_[programFront_];
    const bool ready = program.memory && program.generation == generation;
    const uint32_t active = ready ? generation : 0;
    if (activeGeneration_.load(std::memory_order_relaxed) != active)
        activeGeneration_.store(active, std::memory_order_release);
    if (activate) activate(ready ? &program.dsp : nullptr);
    if (!ready) return false;
    program.entry(context, route, node);
    return true;
}
const audiox::effects::DspTable *NativeGraph::dsp(uint32_t generation) const {
    const auto &program = programs_[programFront_];
    return program.memory && program.generation == generation ? &program.dsp : nullptr;
}
void *NativeGraph::threadMain(void *opaque) {
    auto &self = *static_cast<NativeGraph *>(opaque);
    uint32_t loggedGeneration = 0;
    while (self.running_.load(std::memory_order_acquire)) {
        const uint32_t active = self.activeGeneration_.load(std::memory_order_acquire);
        if (active != loggedGeneration) {
            if (active) std::printf("[GRAPH] [INFO] audio adopted native DSP generation %u\n", active);
            else std::printf("[GRAPH] [WARN] waiting for current native graph; built-in DSP muted\n");
            loggedGeneration = active;
        }
        if (self.requestMiddle_.load(std::memory_order_acquire) & 4U) {
            self.requestFront_ = self.requestMiddle_.exchange(self.requestFront_,
                std::memory_order_acq_rel) & 3U;
            self.compile(self.requests_[self.requestFront_]);
        } else {
            timespec deadline{};
            clock_gettime(CLOCK_REALTIME, &deadline);
            deadline.tv_nsec += 100000000;
            if (deadline.tv_nsec >= 1000000000) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000;
            }
            int result;
            do { result = sem_timedwait(&self.wake_, &deadline); } while (result && errno == EINTR);
            if (result && errno != ETIMEDOUT)
                std::fprintf(stderr, "[GRAPH] [ERR] wait for compiler request: %s\n", std::strerror(errno));
        }
    }
    return nullptr;
}
void NativeGraph::compile(const Schedule &schedule) {
    const auto started = Clock::now();
    if (!schedule.generation || schedule.count > kMaxOperations) {
        std::fprintf(stderr, "[GRAPH] [ERR] invalid schedule; built-in DSP muted\n");
        return;
    }
    std::printf("[GRAPH] [INFO] compiling native DSP generation %u; enabled built-ins muted until ready\n",
                schedule.generation);
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
        char *arguments[]{const_cast<char *>(compiler_), const_cast<char *>(bitcode_), nullptr};
        if (!result) result = posix_spawn(&child, compiler_, &actions, nullptr, arguments, environ);
        posix_spawn_file_actions_destroy(&actions);
        close(sockets[1]);
        if (!result) {
            NativeHeader header{};
            CompileRequest request{};
            request.schedule = schedule;
            Program candidate{};
            candidate.memory = mmap(nullptr, kMaxNativeBytes, PROT_NONE,
                                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (candidate.memory == MAP_FAILED) candidate.memory = nullptr;
            candidate.size = kMaxNativeBytes;
            candidate.generation = schedule.generation;
            request.address = reinterpret_cast<uintptr_t>(candidate.memory);
#define IMPORT(symbol) \
            std::strcpy(request.imports[request.importCount].name, #symbol); \
            request.imports[request.importCount++].address = reinterpret_cast<uintptr_t>(&::symbol)
            IMPORT(memcpy); IMPORT(memmove); IMPORT(memset);
            IMPORT(exp); IMPORT(expf); IMPORT(pow); IMPORT(powf);
            IMPORT(log); IMPORT(logf); IMPORT(log10); IMPORT(log10f);
            IMPORT(sin); IMPORT(sinf); IMPORT(cos); IMPORT(cosf);
            IMPORT(tan); IMPORT(tanf); IMPORT(sincos); IMPORT(sincosf);
            IMPORT(sqrt); IMPORT(sqrtf); IMPORT(atan2); IMPORT(atan2f);
            IMPORT(remainder); IMPORT(remainderf); IMPORT(fmod); IMPORT(fmodf);
            IMPORT(fmaxf); IMPORT(fminf); IMPORT(floorf); IMPORT(ceilf);
            IMPORT(roundf); IMPORT(truncf);
            IMPORT(exp2); IMPORT(exp2f); IMPORT(log2); IMPORT(log2f);
            IMPORT(tanh); IMPORT(tanhf); IMPORT(floor); IMPORT(ceil);
            IMPORT(lround); IMPORT(lroundf); IMPORT(nextafterf);
#undef IMPORT
            const auto deadline = Clock::now() + std::chrono::seconds(10);
            bool ok = candidate.memory &&
                      transfer(sockets[0], &request, sizeof(request), true, deadline) &&
                      transfer(sockets[0], &header, sizeof(header), false, deadline);
            if (ok && (header.magic != kNativeMagic || !header.size || header.size > kMaxNativeBytes)) {
                errno = EINVAL;
                ok = false;
            }
            if (ok) {
                if (header.entryOffset >= header.size) ok = false;
                for (auto offset : header.dspOffsets) {
                    if (offset >= header.size) ok = false;
                }
                if (!ok) errno = EINVAL;
                else {
                    const long pageSize = sysconf(_SC_PAGESIZE);
                    if (pageSize <= 0) {
                        errno = EINVAL;
                        ok = false;
                    } else {
                        const auto retained = static_cast<uint32_t>(
                            (header.size + pageSize - 1) / pageSize * pageSize);
                        if (retained < kMaxNativeBytes &&
                            munmap(static_cast<char *>(candidate.memory) + retained, kMaxNativeBytes - retained)) {
                            ok = false;
                        } else {
                            candidate.size = retained;
                            ok = mprotect(candidate.memory, candidate.size, PROT_READ | PROT_WRITE) == 0 &&
                                 transfer(sockets[0], candidate.memory, header.size, false, deadline);
                        }
                    }
                }
            }
            if (ok) {
                __builtin___clear_cache(static_cast<char *>(candidate.memory),
                                        static_cast<char *>(candidate.memory) + header.size);
                ok = mprotect(candidate.memory, candidate.size, PROT_READ | PROT_EXEC) == 0;
                const auto base = reinterpret_cast<uintptr_t>(candidate.memory);
                candidate.entry = reinterpret_cast<NativeEntry>(base + header.entryOffset);
                for (unsigned i = 0; i < audiox::effects::kBuiltinDspCount; ++i)
                    candidate.dsp.entries[i] = reinterpret_cast<audiox::effects::DspEntry>(base + header.dspOffsets[i]);
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
                std::fprintf(stderr, "[GRAPH] [ERR] compile generation %u: %s; built-in DSP muted\n",
                             schedule.generation, std::strerror(error));
                return;
            }
            clear(programs_[programBack_]);
            programs_[programBack_] = candidate;
            programBack_ = programMiddle_.exchange(programBack_ | 4U,
                                                   std::memory_order_acq_rel) & 3U;
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
                Clock::now() - started).count();
            std::printf("[GRAPH] [INFO] LLVM compiled DSP generation %u: %u operations, %u bytes in %lld us\n",
                        schedule.generation, schedule.count, header.size,
                        static_cast<long long>(elapsed));
            return;
        }
    } else {
        close(sockets[1]);
    }
    close(sockets[0]);
    std::fprintf(stderr, "[GRAPH] [ERR] launch compiler: %s; built-in DSP muted\n",
                 std::strerror(result));
}
} // namespace audiox::graph
