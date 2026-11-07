#include "audio/graph_native.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

using namespace audiox::graph;
static void route(void *opaque, uint32_t index) {
    static_cast<std::vector<unsigned> *>(opaque)->push_back(index);
}
static void node(void *opaque, uint32_t index) {
    static_cast<std::vector<unsigned> *>(opaque)->push_back(1000 + index);
}
static void waitFor(NativeGraph &graph, const Schedule &schedule) {
    std::vector<unsigned> actual;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!graph.execute(schedule.generation, &actual, route, node)) {
        assert(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::vector<unsigned> expected;
    for (unsigned i = 0; i < schedule.count; ++i)
        expected.push_back(schedule.operations[i].index +
            (schedule.operations[i].kind == OperationKind::Node ? 1000 : 0));
    assert(actual == expected);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    NativeGraph graph(argv[1]);
    Schedule schedule{};
    schedule.generation = 1;
    std::vector<unsigned> empty;
    assert(!graph.execute(1, &empty, route, node));
    graph.request(schedule);
    waitFor(graph, schedule);
    for (unsigned generation = 2; generation <= 12; ++generation) {
        schedule.generation = generation;
        schedule.count = generation == 12 ? kMaxOperations : generation;
        for (unsigned i = 0; i < schedule.count; ++i)
            schedule.operations[i] = {i % 3 ? OperationKind::Route : OperationKind::Node,
                                     i % (i % 3 ? 256U : 64U)};
        assert(!graph.execute(generation, &empty, route, node));
        graph.request(schedule);
        waitFor(graph, schedule);
    }
    // Requests can coalesce; execution must never use a different generation.
    for (unsigned generation = 13; generation < 100; ++generation) {
        schedule.generation = generation;
        graph.request(schedule);
    }
    waitFor(graph, schedule);
    {
        NativeGraph unavailable("/nonexistent/audiox-graph-compiler");
        unavailable.request(schedule);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(!unavailable.execute(schedule.generation, &empty, route, node));
    }
    {
        NativeGraph rejected("/bin/false");
        rejected.request(schedule);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        assert(!rejected.execute(schedule.generation, &empty, route, node));
    }
    puts("LLVM native graph: empty/full schedules, operation order, replacement, coalescing and fallback passed");
}
