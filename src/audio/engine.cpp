#include "audio/engine.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace audiox {

AudioEngine::AudioEngine(uint32_t sampleRate, uint32_t channels)
    : sampleRate_(sampleRate), channels_(channels) {
}

uint32_t AudioEngine::sampleRate() const {
    return sampleRate_;
}

uint32_t AudioEngine::channels() const {
    return channels_;
}

size_t AudioEngine::effectCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return effects_.size();
}

int AudioEngine::addEffect(const std::string& effectId, uint8_t type, bool enabled) {
    if (effectId.empty() || type > effects::EFFECT_GATE) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    const auto exists = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (exists != effects_.end()) {
        return -1;
    }

    Effect effect = {};
    effect.id = effectId;
    effects::setSlotDefaultsForType(&effect.params, type);
    effect.params.type = type;
    effect.params.enabled = enabled ? 1U : 0U;
    effects_.push_back(effect);
    return 0;
}

int AudioEngine::removeEffect(const std::string& effectId) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (found == effects_.end()) {
        return -1;
    }
    effects_.erase(found);
    return 0;
}

int AudioEngine::setEffectType(const std::string& effectId, uint8_t type) {
    if (type > effects::EFFECT_GATE) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (found == effects_.end()) {
        return -1;
    }
    const uint8_t enabled = found->params.enabled;
    effects::setSlotDefaultsForType(&found->params, type);
    found->params.type = type;
    found->params.enabled = enabled;
    return 0;
}

int AudioEngine::setEffectEnabled(const std::string& effectId, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (found == effects_.end()) {
        return -1;
    }
    found->params.enabled = enabled ? 1U : 0U;
    return 0;
}

int AudioEngine::setEffectParameter(const std::string& effectId, const char* name, float value) {
    if (!std::isfinite(value)) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    const auto found = std::find_if(effects_.begin(), effects_.end(), [&](const Effect& effect) {
        return effect.id == effectId;
    });
    if (found == effects_.end()) {
        return -1;
    }
    return effects::setSlotParamValue(&found->params, name, value);
}

std::vector<EngineEffectInfo> AudioEngine::effects() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<EngineEffectInfo> result;
    result.reserve(effects_.size());
    for (const Effect& effect : effects_) {
        EngineEffectInfo info = {};
        info.id = effect.id;
        info.type = effect.params.type;
        info.enabled = effect.params.enabled;
        const effects::EffectTypeSpec* spec = effects::effectTypeSpecFor(effect.params.type);
        info.values.assign(effect.params.values, effect.params.values + spec->paramCount);
        result.push_back(info);
    }
    return result;
}

int AudioEngine::process(const float* input, float* output, uint32_t frames) {
    if (!input || !output || frames == 0 || channels_ == 0) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    inputScratch_.resize(frames);
    outputScratch_.resize(frames);

    for (uint32_t channel = 0; channel < channels_; ++channel) {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            inputScratch_[frame] = input[static_cast<size_t>(frame) * channels_ + channel];
        }

        const float* currentInput = inputScratch_.data();
        float* currentOutput = outputScratch_.data();
        for (const Effect& effect : effects_) {
            effects::processSlot(effect.id.c_str(),
                                 static_cast<uint8_t>(channel),
                                 effect.params,
                                 currentInput,
                                 currentOutput,
                                 frames);
            currentInput = currentOutput;
            currentOutput = currentOutput == outputScratch_.data() ? inputScratch_.data() : outputScratch_.data();
        }

        for (uint32_t frame = 0; frame < frames; ++frame) {
            output[static_cast<size_t>(frame) * channels_ + channel] = currentInput[frame];
        }
    }
    return 0;
}

}  // namespace audiox
