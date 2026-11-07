#pragma once

#include <atomic>
#include <cstdint>
#include <pthread.h>
#include <semaphore.h>
#include "audio/effects/native.hpp"
#include "audio/graph_block.hpp"

namespace audiox::graph {

constexpr unsigned kMaxOperations = 320;
constexpr uint32_t kNativeMagic = 0x41584733;
constexpr uint32_t kMaxNativeBytes = 4 * 1024 * 1024;
constexpr unsigned kMaxImports = 48;
struct NativeImport {
    char name[32]{};
    uint64_t address{};
};

enum class OperationKind : uint32_t { Route, Node };
struct Operation {
    OperationKind kind{};
    uint32_t index{};
};
struct Schedule {
    uint32_t generation{};
    uint32_t count{};
    uint32_t frames{128};
    uint32_t sampleRate{48000};
    uint32_t direct{};
    BlockRoute routes[256]{};
    Operation operations[kMaxOperations]{};
};
struct NativeHeader {
    uint32_t magic{};
    uint32_t size{};
    uint32_t entryOffset{};
    uint32_t dspOffsets[audiox::effects::kBuiltinDspCount]{};
};
struct CompileRequest {
    Schedule schedule{};
    uint64_t address{};
    uint32_t importCount{};
    NativeImport imports[kMaxImports]{};
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
        NativeEntry entry{};
        audiox::effects::DspTable dsp{};
    };
    Schedule requests_[3]{};
    std::atomic<unsigned> requestMiddle_{1};
    unsigned requestBack_{2}, requestFront_{0};
    Program programs_[3]{};
    std::atomic<unsigned> programMiddle_{1};
    unsigned programBack_{2}, programFront_{0};
    std::atomic<bool> running_{false};
    std::atomic<uint32_t> activeGeneration_{0};
    sem_t wake_{};
    bool wakeInitialized_{};
    pthread_t thread_{};
    const char *compiler_;
    const char *bitcode_;

    static void *threadMain(void *);
    void compile(const Schedule &);
    void clear(Program &);

  public:
    explicit NativeGraph(const char *compiler, const char *bitcode = "/usr/share/audiox/dsp");
    ~NativeGraph();
    NativeGraph(const NativeGraph &) = delete;
    NativeGraph &operator=(const NativeGraph &) = delete;
    void request(const Schedule &);
    bool ready(uint32_t generation);
    bool execute(uint32_t generation, void *context, Callback route, Callback node,
                 void (*activate)(const audiox::effects::DspTable *) = nullptr);
    const audiox::effects::DspTable *dsp(uint32_t generation) const;
};

} // namespace audiox::graph
