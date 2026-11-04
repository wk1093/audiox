#include "audio/plugin_ipc.hpp"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <fcntl.h>
#include <grp.h>
#include <linux/audit.h>
#include <linux/filter.h>
#include <linux/landlock.h>
#include <linux/seccomp.h>
#include <signal.h>
#include <sched.h>
#include <stddef.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

using namespace audiox::plugin;
namespace {
bool limit(int resource, rlim_t value) {
    const rlimit bound{value, value};
    return setrlimit(resource, &bound) == 0;
}
bool allowPath(int ruleset, const char* path, uint64_t rights, bool optional = false) {
    const int fd = open(path, O_PATH | O_CLOEXEC);
    if (fd < 0) return optional && errno == ENOENT;
    landlock_path_beneath_attr rule{};
    rule.allowed_access = rights; rule.parent_fd = fd;
    const bool ok = syscall(SYS_landlock_add_rule, ruleset,
                           LANDLOCK_RULE_PATH_BENEATH, &rule, 0) == 0;
    close(fd);
    return ok;
}
bool filesystemSandbox(const char* module) {
    const int abi = static_cast<int>(syscall(SYS_landlock_create_ruleset, nullptr, 0,
                                            LANDLOCK_CREATE_RULESET_VERSION));
    if (abi < 1) return false;
    landlock_ruleset_attr rules{};
    rules.handled_access_fs =
        LANDLOCK_ACCESS_FS_EXECUTE | LANDLOCK_ACCESS_FS_WRITE_FILE |
        LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR |
        LANDLOCK_ACCESS_FS_REMOVE_DIR | LANDLOCK_ACCESS_FS_REMOVE_FILE |
        LANDLOCK_ACCESS_FS_MAKE_CHAR | LANDLOCK_ACCESS_FS_MAKE_DIR |
        LANDLOCK_ACCESS_FS_MAKE_REG | LANDLOCK_ACCESS_FS_MAKE_SOCK |
        LANDLOCK_ACCESS_FS_MAKE_FIFO | LANDLOCK_ACCESS_FS_MAKE_BLOCK |
        LANDLOCK_ACCESS_FS_MAKE_SYM;
    // Newer rights are enforced by seccomp on older Landlock kernels too.
#ifdef LANDLOCK_ACCESS_FS_REFER
    if (abi >= 2) rules.handled_access_fs |= LANDLOCK_ACCESS_FS_REFER;
#endif
#ifdef LANDLOCK_ACCESS_FS_TRUNCATE
    if (abi >= 3) rules.handled_access_fs |= LANDLOCK_ACCESS_FS_TRUNCATE;
#endif
    const int fd = static_cast<int>(syscall(SYS_landlock_create_ruleset, &rules, sizeof(rules), 0));
    if (fd < 0) return false;
    const uint64_t library_rights = LANDLOCK_ACCESS_FS_READ_FILE | LANDLOCK_ACCESS_FS_READ_DIR;
    const bool ok = allowPath(fd, module, LANDLOCK_ACCESS_FS_READ_FILE) &&
                    allowPath(fd, "/lib", library_rights, true) &&
                    allowPath(fd, "/lib64", library_rights, true) &&
                    allowPath(fd, "/usr/lib", library_rights, true) &&
                    allowPath(fd, "/usr/lib64", library_rights, true) &&
                    syscall(SYS_landlock_restrict_self, fd, 0) == 0;
    close(fd);
    return ok;
}
bool syscallSandbox(bool loading) {
    sock_filter code[160];
    unsigned n = 0;
    const auto statement = [&](unsigned short op, uint32_t k) {
        code[n++] = BPF_STMT(op, k);
    };
    const auto jump = [&](uint32_t nr, unsigned char yes, unsigned char no) {
        code[n++] = BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, nr, yes, no);
    };
    statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, arch));
#if defined(__aarch64__)
    jump(AUDIT_ARCH_AARCH64, 1, 0);
#elif defined(__x86_64__)
    jump(AUDIT_ARCH_X86_64, 1, 0);
#else
#error Unsupported native plugin worker architecture
#endif
    statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr));
    const auto allow = [&](uint32_t nr) {
        jump(nr, 0, 1); statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    };
    allow(SYS_exit); allow(SYS_exit_group); allow(SYS_rt_sigreturn);
    allow(SYS_clock_gettime); allow(SYS_futex); allow(SYS_sched_yield);
    allow(SYS_nanosleep); allow(SYS_clock_nanosleep);
    allow(SYS_brk); allow(SYS_mmap); allow(SYS_munmap); allow(SYS_mprotect);
    allow(SYS_madvise);
    if (loading) {
        allow(SYS_read); allow(SYS_pread64); allow(SYS_lseek);
        allow(SYS_close); allow(SYS_fstat); allow(SYS_newfstatat);
        allow(SYS_readlinkat); allow(SYS_getrandom);
        allow(SYS_getpid); allow(SYS_gettid);
        allow(SYS_rt_sigaction); allow(SYS_rt_sigprocmask);
        // Needed only to install the tighter post-initialization filter.
        allow(SYS_seccomp);
#ifdef SYS_readlink
        allow(SYS_readlink);
#endif
        // Loader opens are read-only. Landlock limits their reachable paths.
        jump(SYS_openat, 0, 4);
        statement(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, args[2]));
        code[n++] = BPF_JUMP(BPF_JMP | BPF_JSET | BPF_K,
                             O_WRONLY | O_RDWR | O_CREAT | O_TRUNC | O_APPEND |
                             (O_TMPFILE & ~O_DIRECTORY), 0, 1);
        statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
        statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW);
    }
    // Forbidden operations kill the worker: a syscall-violating plugin cannot
    // quietly report success and continue running.
    statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS);
    sock_fprog program{static_cast<unsigned short>(n), code};
    return syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER, 0, &program) == 0;
}
int fail(Mailbox* box, const char* message) {
    std::snprintf(box->error, sizeof(box->error), "%s", message);
    box->state.store(static_cast<uint32_t>(WorkerState::Fault), std::memory_order_release);
    return 1;
}
bool textMatches(const char* value, const char* expected, size_t size) {
    return value && std::strncmp(value, expected, size) == 0 &&
           ::strnlen(expected, size) < size;
}
}
int main(int argc, char** argv) {
    if (argc != 3) return 1;
    const pid_t parent = getppid();
    char* end;
    const long number = std::strtol(argv[1], &end, 10);
    if (*end || number < 3 || number > 1048576) return 1;
    const int fd = static_cast<int>(number);
    struct stat info{};
    if (fstat(fd, &info) || info.st_size != sizeof(Mailbox)) return 1;
    void* memory = mmap(nullptr, sizeof(Mailbox), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (memory == MAP_FAILED) return 1;
    auto* box = static_cast<Mailbox*>(memory);
    if (box->magic != kIpcMagic || !box->max_frames || box->max_frames > kMaxFrames ||
        !box->channels || box->channels > kMaxChannels ||
        box->param_count > AUDIOX_PLUGIN_MAX_PARAMS) return fail(box, "Invalid IPC contract");
    // No ambient parent descriptors, devices, sockets, or stdio survive.
    if (fd > 3 && syscall(SYS_close_range, 3u, static_cast<unsigned>(fd - 1), 0u))
        return fail(box, "Cannot close inherited descriptors");
    if (syscall(SYS_close_range, static_cast<unsigned>(fd), ~0u, 0u))
        return fail(box, "Cannot close inherited descriptors");
    close(0); close(1); close(2);
    const sched_param scheduling{};
    errno = 0;
    const int priority = getpriority(PRIO_PROCESS, 0);
    if (errno || sched_setscheduler(0, SCHED_OTHER, &scheduling) ||
        setpriority(PRIO_PROCESS, 0, std::max(priority, 10)))
        return fail(box, "Cannot lower worker scheduling priority");
    if (!limit(RLIMIT_AS, 64 * 1024 * 1024) || !limit(RLIMIT_CORE, 0) ||
        !limit(RLIMIT_NOFILE, 32))
        return fail(box, "Cannot apply worker resource limits");
    if (geteuid() == 0 && (setgroups(0, nullptr) || setgid(65534) || setuid(65534)))
        return fail(box, "Cannot drop worker privileges");
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent ||
        prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) ||
        !filesystemSandbox(argv[2]) || !syscallSandbox(true))
        return fail(box, "Required plugin sandbox unavailable");
    // Sandbox is active even for module constructors and the ABI entrypoint.
    void* module = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!module) return fail(box, "Cannot load plugin module in sandbox");
    auto entry = reinterpret_cast<AudioxPluginEntryV1>(dlsym(module, "audiox_plugin_v1"));
    if (!entry) return fail(box, "Missing audiox_plugin_v1 export");
    const AudioxPluginV1* api = entry();
    if (!api || api->abi_version != AUDIOX_PLUGIN_ABI_VERSION ||
        api->struct_size < sizeof(AudioxPluginV1) ||
        !textMatches(api->id, box->id, sizeof(box->id)) ||
        api->param_count != box->param_count || (api->param_count && !api->params) ||
        !api->create || !api->destroy || !api->reset || !api->process || !api->tail)
        return fail(box, "Plugin ABI or identifier mismatch");
    for (uint32_t p = 0; p < api->param_count; ++p) {
        const auto& a = api->params[p]; const auto& b = box->expected[p];
        if (!textMatches(a.name, b.name, sizeof(b.name)) ||
            !textMatches(a.label, b.label, sizeof(b.label)) ||
            a.min != b.min || a.max != b.max || a.default_value != b.default_value)
            return fail(box, "Plugin parameter metadata mismatch");
    }
    void* instances[kMaxChannels]{};
    for (uint32_t c = 0; c < box->channels; ++c) {
        instances[c] = api->create(box->sample_rate, box->max_frames);
        if (!instances[c]) return fail(box, "Plugin instance initialization failed");
    }
    if (!syscallSandbox(false)) return fail(box, "Cannot tighten plugin sandbox");
    uint32_t completed = 0, generation = box->generation.load(std::memory_order_acquire);
    box->state.store(static_cast<uint32_t>(WorkerState::Ready), std::memory_order_release);
    for (;;) {
        const uint32_t requested_generation = box->generation.load(std::memory_order_acquire);
        if (requested_generation != generation) {
            for (uint32_t c = 0; c < box->channels; ++c) api->reset(instances[c]);
            generation = requested_generation;
        }
        const uint32_t request = box->request.load(std::memory_order_acquire);
        if (request == completed) {
            const timespec pause{0, 50000};
            nanosleep(&pause, nullptr);
            continue;
        }
        if (!box->frames || box->frames > box->max_frames)
            return fail(box, "Invalid plugin processing block");
        if (generation != box->request_generation) {
            for (uint32_t c = 0; c < box->channels; ++c) api->reset(instances[c]);
            generation = box->request_generation;
        }
        for (uint32_t p = 0; p < api->param_count; ++p)
            box->params[p] = std::clamp(box->params[p], api->params[p].min, api->params[p].max);
        uint32_t tail = 0;
        const uint32_t threshold_bits = box->tail_threshold.load(std::memory_order_relaxed);
        float threshold;
        std::memcpy(&threshold, &threshold_bits, sizeof(threshold));
        if (!std::isfinite(threshold) || threshold < 0) threshold = 0;
        for (uint32_t c = 0; c < box->channels; ++c) {
            api->process(instances[c], box->input + c * box->max_frames,
                         box->output + c * box->max_frames, box->frames, box->params);
            tail |= api->tail(instances[c], threshold) != 0;
        }
        box->tail = tail;
        box->response_generation = box->request_generation;
        completed = request;
        box->response.store(completed, std::memory_order_release);
    }
}
