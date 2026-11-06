#include "system_history.hpp"
#include <cassert>

int main() {
    using namespace audiox::system;
    static_assert(sizeof(SystemHistory) < 65536, "history must stay within 64 KiB");
    SystemHistory history(42);
    CpuTimes cpu{100, 50};
    assert(history.size() == 0 && history.runtime() == 42);
    history.add(1000, 55, &cpu, 10, 1);
    assert(std::isnan(history.latest().cpu) && !history.latest().xrunsValid);
    cpu = {200, 70};
    history.add(1999, 55, &cpu, 11, 1);
    assert(history.size() == 1);
    history.add(2000, 56, &cpu, 12, 1);
    assert(history.latest().cpu == 80 && history.latest().xruns == 2 && history.latest().xrunsValid);
    history.add(10000, 60, &cpu, 20, 1);
    assert(std::isnan(history.latest().cpu) && !history.latest().xrunsValid);
    cpu = {300, 90};
    history.add(11000, 61, &cpu, 0, 2);
    assert(!history.latest().xrunsValid && history.latest().cpu == 80);
    history.add(12000, NAN, nullptr, 1, 2);
    assert(std::isnan(history.latest().temperature) && std::isnan(history.latest().cpu));
    assert(history.latest().xrunsValid && history.latest().xruns == 1);
    history.add(13000, 60, &cpu, 2, 2);
    assert(std::isnan(history.latest().cpu));
    cpu = {1, 0};
    history.add(14000, 60, &cpu, 1, 2);
    assert(std::isnan(history.latest().cpu) && !history.latest().xrunsValid);
    for (uint64_t i = 0; i < 2000; ++i) {
        cpu.total += 100; cpu.idle += 25;
        history.add(15000 + i * 1000, 62, &cpu, i + 2, 2);
    }
    assert(history.size() == 600);
    assert(history.at(0).id == history.latest().id - 599);
    assert(history.latest().cpu == 75);
    for (size_t i = 1; i < history.size(); ++i)
        assert(history.at(i).now - history.at(i - 1).now == 1000);
    assert(history.latest().now - history.at(0).now == 599000);
    const uint64_t last = history.latest().now;
    history.prune(last + 599999);
    assert(history.size() == 1);
    history.prune(last + 600000);
    assert(history.size() == 0);
    history.add(last + 601000, 62, &cpu, 2003, 2);
    assert(history.size() == 1 && !history.latest().xrunsValid && std::isnan(history.latest().cpu));

    SystemHistory sparse;
    sparse.add(1000, 55, &cpu, 0, 1);
    sparse.add(600999, 55, &cpu, 0, 1);
    assert(sparse.size() == 2);
    sparse.add(601999, 55, &cpu, 0, 1);
    assert(sparse.size() == 2 && sparse.at(0).now == 600999);
    const auto size = audiox::system::systemHistory.size();
    pollSystemHistory(audiox::healthMonotonicMs());
    assert(audiox::system::systemHistory.size() == size + 1);
}
