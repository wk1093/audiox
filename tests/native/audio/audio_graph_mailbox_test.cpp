#include "audio/context.hpp"

#include <cassert>
#include <cstring>
#include <thread>

int main() {
    auto mailbox = std::make_unique<AudioGraphMailbox>();
    AudioGraphState snapshot{};
    assert(!mailbox->consume(&snapshot));
    std::mutex writers;
    std::atomic<unsigned> finished{0};
    uint32_t generation = 0;
    constexpr unsigned updatesPerWriter = 50000;
    auto publish = [&] {
        AudioGraphState graph{};
        for (unsigned i = 0; i < updatesPerWriter; ++i) {
            std::lock_guard<std::mutex> lock(writers);
            ++generation;
            memset(&graph, generation & 0x7f, sizeof(graph));
            graph.generation = generation;
            mailbox->publish(graph);
        }
        finished.fetch_add(1, std::memory_order_release);
    };
    std::thread first(publish);
    std::thread second(publish);
    uint32_t last = 0;
    AudioGraphState expected{};
    while (finished.load(std::memory_order_acquire) != 2 || last != updatesPerWriter * 2) {
        if (!mailbox->consume(&snapshot)) {
            std::this_thread::yield();
            continue;
        }
        assert(snapshot.generation > last);
        last = snapshot.generation;
        memset(&expected, last & 0x7f, sizeof(expected));
        expected.generation = last;
        assert(memcmp(&snapshot, &expected, sizeof(snapshot)) == 0);
        if (last % 17 == 0) std::this_thread::yield();
    }
    first.join();
    second.join();
    assert(!mailbox->consume(&snapshot));
    assert(mailbox->consume(&snapshot, true));
    assert(snapshot.generation == updatesPerWriter * 2);
    puts("Graph mailbox: 100000 coherent updates, two serialized publishers, one lock-free reader");
}
