#pragma once

#include "audio/health.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>
#include <string_view>

namespace audiox::system {

struct CpuTimes {
    uint64_t total{0}, idle{0};
};

inline bool readCpuTimes(CpuTimes &times) {
    FILE *file = fopen("/proc/stat", "r");
    if (!file) return false;
    char label[8]{};
    unsigned long long user = 0, nice = 0, system = 0, idle = 0;
    unsigned long long iowait = 0, irq = 0, softirq = 0, steal = 0;
    const int count = fscanf(file, "%7s %llu %llu %llu %llu %llu %llu %llu %llu",
                             label, &user, &nice, &system, &idle, &iowait, &irq, &softirq, &steal);
    fclose(file);
    if (count != 9 || std::string_view(label) != "cpu") return false;
    times = {user + nice + system + idle + iowait + irq + softirq + steal, idle + iowait};
    return true;
}

inline double readCpuTemperature() {
    double result = std::numeric_limits<double>::quiet_NaN();
    FILE *file = fopen("/sys/class/thermal/thermal_zone0/temp", "r");
    if (!file) return result;
    long milli = -1;
    if (fscanf(file, "%ld", &milli) == 1 && milli >= 0 && milli <= 150000)
        result = milli / 1000.0;
    fclose(file);
    return result;
}

class SystemHistory {
public:
    static constexpr size_t kCapacity = 1000;
    static constexpr uint64_t kIntervalMs = 1000;
    struct Sample {
        uint64_t id{0}, now{0};
        double temperature{0}, cpu{0};
        uint64_t xruns{0};
        bool xrunsValid{false};
    };

    explicit SystemHistory(uint64_t runtime = healthMonotonicMs()) : runtime_(runtime) {}

    bool due(uint64_t now) const {
        return !size_ || (now > latest().now && now - latest().now >= kIntervalMs);
    }

    void add(uint64_t now, double temperature, const CpuTimes *cpu,
             uint64_t xruns, uint64_t audioRuntime) {
        if (!due(now)) return;
        Sample sample;
        sample.id = ++sequence_;
        sample.now = now;
        sample.temperature = temperature;
        sample.cpu = std::numeric_limits<double>::quiet_NaN();
        const bool continuous = size_ && now - latest().now <= 5000;
        if (continuous && cpu && previousCpuValid_ && cpu->total > previousCpu_.total &&
            cpu->idle >= previousCpu_.idle) {
            const uint64_t total = cpu->total - previousCpu_.total;
            const uint64_t idle = cpu->idle - previousCpu_.idle;
            if (idle <= total) sample.cpu = 100.0 * (total - idle) / total;
        }
        sample.xrunsValid = continuous && audioRuntime == previousAudioRuntime_ && xruns >= previousXruns_;
        if (sample.xrunsValid) sample.xruns = xruns - previousXruns_;
        samples_[next_] = sample;
        next_ = (next_ + 1) % kCapacity;
        size_ = std::min(size_ + 1, kCapacity);
        previousCpuValid_ = cpu != nullptr;
        if (cpu) previousCpu_ = *cpu;
        previousXruns_ = xruns;
        previousAudioRuntime_ = audioRuntime;
    }

    size_t size() const { return size_; }
    uint64_t runtime() const { return runtime_; }
    const Sample &at(size_t index) const { return samples_[(next_ + kCapacity - size_ + index) % kCapacity]; }
    const Sample &latest() const { return at(size_ - 1); }

private:
    std::array<Sample, kCapacity> samples_{};
    size_t next_{0}, size_{0};
    uint64_t sequence_{0}, runtime_, previousXruns_{0}, previousAudioRuntime_{0};
    CpuTimes previousCpu_;
    bool previousCpuValid_{false};
};

inline SystemHistory systemHistory;

inline void pollSystemHistory(uint64_t now) {
    if (!systemHistory.due(now)) return;
    CpuTimes cpu;
    const bool cpuAvailable = readCpuTimes(cpu);
    const double temperature = readCpuTemperature();
    if (!cpuAvailable || !std::isfinite(temperature)) {
        static bool warned = false;
        if (!warned) printf("[SYSTEM] [WARN] health sampling: CPU counters or temperature unavailable\n");
        warned = true;
    }
    systemHistory.add(now, temperature, cpuAvailable ? &cpu : nullptr,
                      audioHealth.total(), adaptiveAudioBuffer.instanceId());
}

} // namespace audiox::system
