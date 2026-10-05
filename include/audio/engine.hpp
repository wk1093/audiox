#pragma once

#include "audio/effects/slot.hpp"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace audiox {

struct EngineEffectInfo {
    std::string id;
    uint8_t type;
    uint8_t enabled;
    std::vector<float> values;
};

class AudioEngine {
public:
    static constexpr size_t kMaximumEffects = 62;

    AudioEngine(uint32_t sampleRate, uint32_t channels);

    uint32_t sampleRate() const;
    uint32_t channels() const;
    size_t effectCount() const;

    int addEffect(const std::string& effectId, uint8_t type, bool enabled = true);
    int removeEffect(const std::string& effectId);
    int setEffectType(const std::string& effectId, uint8_t type);
    int setEffectEnabled(const std::string& effectId, bool enabled);
    int setEffectParameter(const std::string& effectId, const char* name, float value);
    std::vector<EngineEffectInfo> effects() const;

    int process(const float* input, float* output, uint32_t frames);

private:
    struct Effect {
        std::string id;
        effects::SlotParams params;
    };

    uint32_t sampleRate_;
    uint32_t channels_;
    std::vector<Effect> effects_;
    mutable std::mutex mutex_;
};

}  // namespace audiox
