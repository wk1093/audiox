#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <string>

struct Audiox;

namespace audiox::telemetry {

constexpr size_t kBatchSize = 100;
constexpr size_t kUploadSize = 1000;
constexpr size_t kCapacity = 8640;
constexpr uint64_t kIntervalMs = 10000;
constexpr size_t kEffectTypes = 12; // Eleven built-ins, then all third-party plugins.

enum Metric : size_t {
    UsbAudio, GadgetAudio, OtherAudio, BluetoothAudio, MidiConnected,
    InputDevices, OutputDevices, SoundboardTriggers, MidiNotes, MidiCc, SceneRecalls,
    SamplerEnabled, Xruns, SampleRate, BlockFrames, TemperatureC, CpuPercent, AudioActive, MetricCount
};
inline constexpr const char *kMetricNames[] = {
    "usb_audio", "gadget_audio", "other_audio", "bluetooth_audio",
    "midi_connected", "input_devices", "output_devices", "soundboard_triggers",
    "midi_notes", "midi_cc", "scene_recalls", "sampler_enabled", "xruns",
    "sample_rate", "block_frames", "temperature_c", "cpu_percent", "audio_active"
};
static_assert(sizeof(kMetricNames) / sizeof(*kMetricNames) == MetricCount);

struct Point {
    std::string id;
    std::string installationId;
    std::string version;
    unsigned board{0};
    std::array<int64_t, MetricCount> metrics{};
    std::array<std::array<unsigned, 2>, kEffectTypes> effects{};
    std::string json() const;
};

enum class Consent { Unknown, Disabled, Enabled };

class IdleFilter {
public:
    bool keep(const Point &point, bool configurationChanged) {
        bool active = point.metrics[AudioActive] || point.metrics[SoundboardTriggers] ||
            point.metrics[MidiNotes] || point.metrics[MidiCc] || point.metrics[SceneRecalls] ||
            point.metrics[Xruns];
        bool changed = !initialized_ || configurationChanged || point.version != previous_.version ||
            point.board != previous_.board || point.effects != previous_.effects;
        for (size_t i = 0; i < MetricCount; ++i) {
            if (i == TemperatureC || i == CpuPercent || i == AudioActive ||
                i == SoundboardTriggers || i == MidiNotes || i == MidiCc ||
                i == SceneRecalls || i == Xruns) continue;
            changed |= point.metrics[i] != previous_.metrics[i];
        }
        previous_ = point;
        initialized_ = true;
        return active || changed;
    }
private:
    Point previous_;
    bool initialized_{false};
};

// Owned by the main control loop, like SystemHistory; never used by the audio thread.
class Store {
public:
    explicit Store(std::string directory);
    bool load();
    bool setConsent(bool enabled);
    bool append(Point point);
    bool acknowledge(const std::string &batch);
    std::string status() const;
    std::string batch() const;
    Consent consent() const { return consent_; }
    size_t size() const { return points_.size(); }
    const std::string &error() const { return error_; }
private:
    bool fail(const char *operation);
    bool rewrite(const std::deque<Point> &points);
    bool atomicWrite(const std::string &path, const std::string &text);
    bool randomId(std::string &out, size_t bytes);
    bool loadInstallationId();
    bool removeFile(const char *name);
    std::string token() const;
    std::string directory_;
    std::deque<Point> points_;
    Consent consent_{Consent::Unknown};
    std::string error_;
    std::string installationId_;
    bool loaded_{false};
};

extern Store store;
void poll(Audiox &app, uint64_t now);

} // namespace audiox::telemetry
