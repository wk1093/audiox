#include "audio/engine.hpp"

#include <cmath>
#include <cstdio>
#include <limits>

namespace {

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

}  // namespace

int main() {
    audiox::AudioEngine engine(48000, 2);
    if (engine.addEffect("gain", audiox::effects::EFFECT_GAIN) != 0 ||
        engine.setEffectParameter("gain", "level", 2.0f) != 0) {
        std::fprintf(stderr, "could not configure gain effect\n");
        return 1;
    }

    const float input[] = {0.1f, -0.2f, 0.3f, -0.4f};
    float output[4] = {};
    if (engine.process(input, output, 2) != 0 ||
        !near(output[0], 0.2f) || !near(output[1], -0.4f) ||
        !near(output[2], 0.6f) || !near(output[3], -0.8f)) {
        std::fprintf(stderr, "effect chain did not process interleaved channels correctly\n");
        return 1;
    }

    if (engine.setEffectEnabled("gain", false) != 0 || engine.process(input, output, 2) != 0 ||
        !near(output[0], input[0]) || !near(output[1], input[1])) {
        std::fprintf(stderr, "bypass did not pass input unchanged\n");
        return 1;
    }

    if (engine.setEffectEnabled("gain", true) != 0 ||
        engine.setEffectParameter("gain", "unknown", 1.0f) == 0 ||
        engine.removeEffect("gain") != 0 || engine.effectCount() != 0) {
        std::fprintf(stderr, "effect control operations did not maintain chain state\n");
        return 1;
    }

    if (engine.addEffect("drive", audiox::effects::EFFECT_GAIN, false) != 0 ||
        engine.setEffectType("drive", audiox::effects::EFFECT_DISTORTION) != 0 ||
        engine.setEffectParameter("drive", "drive", 6.0f) != 0 ||
        engine.setEffectParameter("drive", "drive", std::numeric_limits<float>::quiet_NaN()) == 0) {
        std::fprintf(stderr, "effect type changes did not install the selected parameter set\n");
        return 1;
    }
    const auto effects = engine.effects();
    if (effects.size() != 1 || effects[0].type != audiox::effects::EFFECT_DISTORTION ||
        !near(effects[0].values[1], 6.0f)) {
        std::fprintf(stderr, "effect type or parameter value was not retained\n");
        return 1;
    }

    std::puts("audio engine block and live-control tests passed");
    return 0;
}
