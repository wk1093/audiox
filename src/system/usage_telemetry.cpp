#include "usage_telemetry.hpp"
#include "audio/context.hpp"
#include "audio/health.hpp"
#include "bluetooth/context.hpp"
#include "midi/context.hpp"
#include "http/context.hpp"
#include "system_history.hpp"

#include <algorithm>

namespace audiox::telemetry {

void poll(Audiox &app, uint64_t now) {
    store.load();
    static uint64_t previousMs = 0, xruns = 0;
    static uint32_t triggers = 0, notes = 0, cc = 0, scenes = 0;
    static IdleFilter idleFilter;
    static bool signalObserved = false, configurationChanged = false;
    static uint32_t effectSequence = 0, deviceSequence = 0, graphSequence = 0;
    static std::unordered_map<std::string, float> previousGains;
    if (store.consent() != Consent::Enabled || !store.error().empty()) {
        previousMs = 0;
        idleFilter = {};
        signalObserved = configurationChanged = false;
        return;
    }
    if (!app.audio || !app.midi || !app.bluetooth) return;
    auto &audio = *app.audio;
    auto &midi = *app.midi;
    uint32_t currentGraph;
    {
        std::lock_guard<std::mutex> lock(audio.routingGraphMutex);
        currentGraph = audio.routingGraph.generation;
        if (previousMs && !signalObserved) {
            // Existing peak meters are sampled by the control loop, not the audio callback.
            for (size_t node = 0; node < audio.routingGraph.thingCount; ++node) {
                const auto &thing = audio.routingGraph.things[node];
                const unsigned channels = std::min<unsigned>(thing.outputs ? thing.outputs : thing.inputs, 16);
                for (unsigned channel = 0; channel < channels; ++channel) {
                    if (audio.nodeChannelLevels[node][channel].load(std::memory_order_relaxed) > 0.000031623f) {
                        signalObserved = true; // Approximately -90 dBFS.
                        break;
                    }
                }
                if (signalObserved) break;
            }
        }
    }
    if (previousMs) signalObserved |= audio.sfxIsPlaying.load(std::memory_order_relaxed) != 0;
    const uint32_t currentEffects = audio.effectStatesSeq.load();
    const uint32_t currentDevices = audio.deviceGeneration.load();
    if (previousMs) configurationChanged |= currentEffects != effectSequence ||
        currentDevices != deviceSequence || currentGraph != graphSequence;
    effectSequence = currentEffects;
    deviceSequence = currentDevices;
    graphSequence = currentGraph;
    const uint64_t currentXruns = audioHealth.total();
    const uint32_t currentTriggers = audio.sfxTriggerSeq.load();
    const uint32_t currentScenes = audio.sceneRecallSeq.load();
    if (previousMs && now - previousMs < kIntervalMs) return;
    const auto bounded = [](uint64_t value) { return static_cast<int64_t>(std::min<uint64_t>(value, UINT32_MAX)); };
    if (previousMs && now >= previousMs && now - previousMs <= 2 * kIntervalMs) {
        Point point;
#ifdef AUDIOX_VERSION_STRING
        point.version = AUDIOX_VERSION_STRING;
#else
        point.version = "dev";
#endif
        const char *board = audioxDetectBoard();
        point.board = strcmp(board, "pi4") == 0 ? 4 : strcmp(board, "pi5") == 0 ? 5 : 0;
        auto &m = point.metrics;
        m[SoundboardTriggers] = static_cast<uint32_t>(currentTriggers - triggers);
        m[MidiNotes] = static_cast<uint32_t>(midi.lastNoteSeq - notes);
        m[MidiCc] = static_cast<uint32_t>(midi.lastCcSeq - cc);
        m[SceneRecalls] = static_cast<uint32_t>(currentScenes - scenes);
        m[Xruns] = currentXruns >= xruns ? bounded(currentXruns - xruns) : 0;
        m[SamplerEnabled] = midi.samplerModeActive ? 1 : 0;
        m[AudioActive] = signalObserved ? 1 : 0;
        m[MidiConnected] = midi.connected ? 1 : 0;
        m[SampleRate] = SAMPLE_RATE;
        m[BlockFrames] = BUFFER_FRAMES;
        m[TemperatureC] = m[CpuPercent] = -1;
        const auto &history = audiox::system::systemHistory;
        if (history.size() && now >= history.latest().now && now - history.latest().now <= 5000) {
            const auto &sample = history.latest();
            if (std::isfinite(sample.temperature)) m[TemperatureC] = std::clamp<int64_t>(std::lround(sample.temperature), 0, 150);
            if (std::isfinite(sample.cpu)) m[CpuPercent] = std::clamp<int64_t>(std::lround(sample.cpu), 0, 100);
        }
        // ALSA exposes capture/playback endpoints separately. Count each physical card once.
        {
            std::lock_guard<std::mutex> lock(audio.devicesMutex);
            struct Card { bool usb{}, gadget{}, input{}, output{}; };
            std::unordered_map<uint32_t, Card> cards;
            for (const auto &entry : audio.devices) {
                const auto &device = entry.second;
                auto &card = cards[device.cardIndex];
                card.usb |= device.isUsb != 0;
                card.gadget |= device.isGadget != 0;
                card.input |= device.hasCapture != 0;
                card.output |= device.hasPlayback != 0;
            }
            for (const auto &entry : cards) {
                const auto &card = entry.second;
                ++m[card.gadget ? GadgetAudio : card.usb ? UsbAudio : OtherAudio];
                m[InputDevices] += card.input;
                m[OutputDevices] += card.output;
            }
        }
        BluetoothRoutingDevice bluetooth[BT_MAX_DEVICES];
        const size_t count = app.bluetooth->copyRoutingDevices(bluetooth, BT_MAX_DEVICES);
        for (size_t i = 0; i < count; ++i) {
            if (!bluetooth[i].connected) continue;
            ++m[BluetoothAudio];
            m[InputDevices] += bluetooth[i].input;
            m[OutputDevices] += bluetooth[i].output;
        }
        {
            std::lock_guard<std::mutex> lock(audio.effectsMutex);
            for (const auto &entry : audio.effectStates) {
                const auto &effect = entry.second;
                const size_t type = effect.type <= audiox::effects::EFFECT_NOISE_REDUCTION ? effect.type : kEffectTypes - 1;
                ++point.effects[type][0];
                point.effects[type][1] += effect.enabled ? 1 : 0;
            }
        }
        {
            std::lock_guard<std::mutex> lock(audio.gainsMutex);
            configurationChanged |= audio.thingGains != previousGains;
            previousGains = audio.thingGains;
        }
        if (idleFilter.keep(point, configurationChanged)) store.append(std::move(point));
    } else {
        idleFilter = {};
        std::lock_guard<std::mutex> lock(audio.gainsMutex);
        previousGains = audio.thingGains;
    }
    signalObserved = configurationChanged = false;
    previousMs = now;
    xruns = currentXruns;
    triggers = currentTriggers;
    notes = midi.lastNoteSeq;
    cc = midi.lastCcSeq;
    scenes = currentScenes;
}

} // namespace audiox::telemetry
