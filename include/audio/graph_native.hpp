#pragma once

#include <atomic>
#include <cstdint>
#include <pthread.h>

namespace audiox::graph {

constexpr unsigned kMaxOperations = 320;
constexpr uint32_t kNativeMagic = 0x41584731;
constexpr uint32_t kMaxNativeBytes = 128 * 1024;

enum class OperationKind : uint32_t { Route, Node };
struct Operation {
    OperationKind kind{};
    uint32_t index{};
};
struct Schedule {
    uint32_t generation{};
    uint32_t count{};
    Operation operations[kMaxOperations]{};
};
struct NativeHeader {
    uint32_t magic{};
    uint32_t size{};
};
using Callback = void (*)(void *, uint32_t);
using NativeEntry = void (*)(void *, Callback, Callback);

// Only the audio thread calls request/execute; the compiler owns the other
// mailbox sides. Each side owns its slot until the next exchange.
class NativeGraph {
    static_assert(std::atomic<unsigned>::is_always_lock_free);
    struct Program {
        void *memory{};
        uint32_t size{};
        uint32_t generation{};
    };
    Schedule requests_[3]{};
    std::atomic<unsigned> requestMiddle_{1};
    unsigned requestBack_{2}, requestFront_{0};
    Program programs_[3]{};
    std::atomic<unsigned> programMiddle_{1};
    unsigned programBack_{2}, programFront_{0};
    std::atomic<bool> running_{false};
    pthread_t thread_{};
    const char *compiler_;

    static void *threadMain(void *);
    void compile(const Schedule &);
    void clear(Program &);

  public:
    explicit NativeGraph(const char *compiler);
    ~NativeGraph();
    NativeGraph(const NativeGraph &) = delete;
    NativeGraph &operator=(const NativeGraph &) = delete;
    void request(const Schedule &);
    bool execute(uint32_t generation, void *context, Callback route, Callback node);
};

} // namespace audiox::graph
