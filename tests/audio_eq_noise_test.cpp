#include "audio/effects/equalizer.hpp"
#include "audio/effects/noise_reduction.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr float kPi = 3.14159265358979323846f;

std::vector<float> makeTone(uint32_t frames, float sampleRate, float frequency, float amplitude) {
    std::vector<float> samples(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        samples[i] =
            amplitude * std::sin(2.0f * kPi * frequency * static_cast<float>(i) / sampleRate);
    }
    return samples;
}

float rms(const std::vector<float> &samples, uint32_t first, uint32_t last) {
    double sum = 0.0;
    for (uint32_t i = first; i < last; ++i) {
        sum += static_cast<double>(samples[i]) * samples[i];
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(last - first)));
}

float sineAmplitude(const std::vector<float> &samples, uint32_t first, uint32_t last,
                    float frequency, float sampleRate, uint32_t delay = 0) {
    double inPhase = 0.0;
    double quadrature = 0.0;
    for (uint32_t i = first; i < last; ++i) {
        const float phase = 2.0f * kPi * frequency * static_cast<float>(i - delay) / sampleRate;
        inPhase += static_cast<double>(samples[i]) * std::sin(phase);
        quadrature += static_cast<double>(samples[i]) * std::cos(phase);
    }
    const double count = static_cast<double>(last - first);
    return static_cast<float>(2.0 * std::sqrt(inPhase * inPhase + quadrature * quadrature) / count);
}

void testEqualizerIdentityAndResponse() {
    constexpr uint32_t sampleRate = 48000;
    constexpr uint32_t frames = sampleRate;
    auto input = makeTone(frames, static_cast<float>(sampleRate), 150.0f, 0.2f);
    std::vector<float> output(frames);
    audiox::effects::EqualizerState state{};
    audiox::effects::EqualizerParams params;
    audiox::effects::processEqualizer(
        input.data(), output.data(), frames, params.lowHz, params.lowGainDb, params.midHz,
        params.midGainDb, params.highHz, params.highGainDb, params.q, params.outputGain, &state);
    assert(input == output);

    params.lowGainDb = 6.0f;
    audiox::effects::resetEqualizer(&state);
    audiox::effects::processEqualizer(
        input.data(), output.data(), frames, params.lowHz, params.lowGainDb, params.midHz,
        params.midGainDb, params.highHz, params.highGainDb, params.q, params.outputGain, &state);
    const float lowGain =
        sineAmplitude(output, 12000, frames, 150.0f, static_cast<float>(sampleRate)) /
        sineAmplitude(input, 12000, frames, 150.0f, static_cast<float>(sampleRate));
    assert(lowGain > 1.8f && lowGain < 2.2f);

    input = makeTone(frames, static_cast<float>(sampleRate), 1000.0f, 0.2f);
    params = {};
    params.midGainDb = -9.0f;
    audiox::effects::resetEqualizer(&state);
    audiox::effects::processEqualizer(
        input.data(), output.data(), frames, params.lowHz, params.lowGainDb, params.midHz,
        params.midGainDb, params.highHz, params.highGainDb, params.q, params.outputGain, &state);
    const float midGain =
        sineAmplitude(output, 12000, frames, 1000.0f, static_cast<float>(sampleRate)) /
        sineAmplitude(input, 12000, frames, 1000.0f, static_cast<float>(sampleRate));
    assert(midGain > 0.32f && midGain < 0.42f);

    input = makeTone(frames, static_cast<float>(sampleRate), 6000.0f, 0.2f);
    params = {};
    params.highGainDb = 4.0f;
    params.outputGain = 0.5f;
    audiox::effects::resetEqualizer(&state);
    audiox::effects::processEqualizer(
        input.data(), output.data(), frames, params.lowHz, params.lowGainDb, params.midHz,
        params.midGainDb, params.highHz, params.highGainDb, params.q, params.outputGain, &state);
    const float highGain =
        sineAmplitude(output, 12000, frames, 6000.0f, static_cast<float>(sampleRate)) /
        sineAmplitude(input, 12000, frames, 6000.0f, static_cast<float>(sampleRate));
    assert(highGain > 0.75f && highGain < 0.88f);

    params.lowGainDb = 100.0f;
    params.midHz = -100.0f;
    params.q = 0.0f;
    audiox::effects::processEqualizer(
        input.data(), output.data(), frames, params.lowHz, params.lowGainDb, params.midHz,
        params.midGainDb, params.highHz, params.highGainDb, params.q, params.outputGain, &state);
    for (float sample : output) {
        assert(std::isfinite(sample));
        assert(std::fabs(sample) < 10.0f);
    }
}

void testEqualizerTransitionsAndPending() {
    audiox::effects::EqualizerState state{};
    audiox::effects::EqualizerParams params;
    params.midGainDb = 12.0f;
    float impulse[64]{};
    float output[64]{};
    impulse[0] = 1.0f;
    audiox::effects::processEqualizer(impulse, output, 64, params.lowHz, params.lowGainDb,
                                      params.midHz, params.midGainDb, params.highHz,
                                      params.highGainDb, params.q, params.outputGain, &state);
    assert(audiox::effects::equalizerHasPendingAudio(state));

    params.midGainDb = -18.0f;
    for (unsigned block = 0; block < 20; ++block) {
        audiox::effects::processEqualizer(impulse, output, 64, params.lowHz, params.lowGainDb,
                                          params.midHz, params.midGainDb, params.highHz,
                                          params.highGainDb, params.q, params.outputGain, &state);
        for (float sample : output) {
            assert(std::isfinite(sample));
        }
    }

    audiox::effects::resetEqualizer(&state);
    assert(!audiox::effects::equalizerHasPendingAudio(state));
    audiox::effects::processEqualizer(nullptr, output, 64, params.lowHz, params.lowGainDb,
                                      params.midHz, params.midGainDb, params.highHz,
                                      params.highGainDb, params.q, params.outputGain, &state);
    assert(!audiox::effects::equalizerHasPendingAudio(state));

    float inPlace[64]{};
    inPlace[0] = 0.25f;
    audiox::effects::resetEqualizer(&state);
    audiox::effects::processEqualizer(inPlace, inPlace, 64, 150.0f, 3.0f, 1000.0f, -2.0f, 6000.0f,
                                      1.0f, 1.0f, 1.0f, &state);
    for (float sample : inPlace) {
        assert(std::isfinite(sample));
    }
}

void testNoiseReductionDelayAndPartitioning() {
    constexpr uint32_t frames = 1200;
    audiox::effects::NoiseReductionParams params;
    params.reductionDb = 0.0f;
    std::vector<float> input(frames);
    for (uint32_t i = 0; i < frames; ++i) {
        input[i] = std::sin(static_cast<float>(i) * 0.037f) * 0.3f;
    }
    std::vector<float> full(frames, 0.0f);
    std::vector<float> chunks(frames, 0.0f);
    audiox::effects::NoiseReductionState fullState{};
    audiox::effects::NoiseReductionState chunkState{};
    audiox::effects::processNoiseReduction(input.data(), full.data(), frames, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &fullState);
    uint32_t offset = 0;
    const uint32_t sizes[] = {1, 17, 64, 3, 256, 91, 2, 128};
    unsigned sizeIndex = 0;
    while (offset < frames) {
        const uint32_t count =
            std::min(sizes[sizeIndex++ % (sizeof(sizes) / sizeof(sizes[0]))], frames - offset);
        audiox::effects::processNoiseReduction(input.data() + offset, chunks.data() + offset, count,
                                               params.reductionDb, params.noiseFloorDb,
                                               params.adaptation, params.smoothing, params.mix,
                                               params.outputGain, &chunkState);
        offset += count;
    }
    assert(full == chunks);
    assert(audiox::effects::noiseReductionLatencyFrames() == 256);
    for (uint32_t i = 0; i < full.size(); ++i) {
        const float expected = i < 256 ? 0.0f : input[i - 256];
        assert(full[i] == expected);
    }

    audiox::effects::resetNoiseReduction(&fullState);
    assert(!audiox::effects::noiseReductionHasPendingAudio(fullState));
    float silence[512]{};
    float silentOutput[512]{};
    audiox::effects::processNoiseReduction(silence, silentOutput, 512, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &fullState);
    for (float sample : silentOutput) {
        assert(sample == 0.0f);
    }
    assert(!audiox::effects::noiseReductionHasPendingAudio(fullState));
}

void testNoiseReductionAttenuatesStationaryNoiseAndKeepsTone() {
    constexpr uint32_t sampleRate = 16000;
    constexpr uint32_t frames = sampleRate * 4U;
    std::vector<float> input(frames);
    uint32_t random = 0x12345678U;
    for (uint32_t i = 0; i < frames; ++i) {
        random = random * 1664525U + 1013904223U;
        const float noise =
            (static_cast<float>((random >> 8U) & 0xFFFFU) / 32767.5f - 1.0f) * 0.06f;
        const float voiceTone = 0.2f * std::sin(2.0f * kPi * 1200.0f * static_cast<float>(i) /
                                                static_cast<float>(sampleRate));
        input[i] = noise + voiceTone;
    }

    audiox::effects::NoiseReductionParams params;
    params.reductionDb = 18.0f;
    params.noiseFloorDb = -75.0f;
    params.adaptation = 0.2f;
    params.smoothing = 0.4f;
    std::vector<float> output(frames, 0.0f);
    audiox::effects::NoiseReductionState state{};
    audiox::effects::processNoiseReduction(input.data(), output.data(), frames, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &state);

    const uint32_t start = sampleRate * 2U;
    const uint32_t end = frames - 256U;
    const float toneInput =
        sineAmplitude(input, start, end, 1200.0f, static_cast<float>(sampleRate));
    const float toneOutput =
        sineAmplitude(output, start, end, 1200.0f, static_cast<float>(sampleRate), 256U);
    assert(toneOutput > toneInput * 0.72f);

    std::vector<float> residualInput(end - start);
    std::vector<float> residualOutput(end - start);
    for (uint32_t i = start; i < end; ++i) {
        const float phaseIn =
            2.0f * kPi * 1200.0f * static_cast<float>(i) / static_cast<float>(sampleRate);
        const float phaseOut =
            2.0f * kPi * 1200.0f * static_cast<float>(i - 256U) / static_cast<float>(sampleRate);
        residualInput[i - start] = input[i] - toneInput * std::sin(phaseIn);
        residualOutput[i - start] = output[i] - toneOutput * std::sin(phaseOut);
    }
    assert(rms(residualOutput, 0, static_cast<uint32_t>(residualOutput.size())) <
           rms(residualInput, 0, static_cast<uint32_t>(residualInput.size())) * 0.85f);

    float silence[1024]{};
    float tail[1024]{};
    audiox::effects::processNoiseReduction(silence, tail, 1024, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &state);
    for (float sample : tail) {
        assert(std::isfinite(sample));
    }
    assert(!audiox::effects::noiseReductionHasPendingAudio(state));
    audiox::effects::resetNoiseReduction(&state);
    assert(!audiox::effects::noiseReductionHasPendingAudio(state));
}

void testNoiseReductionChannelIsolationAndControls() {
    audiox::effects::NoiseReductionState left{};
    audiox::effects::NoiseReductionState right{};
    audiox::effects::NoiseReductionParams params;
    params.mix = 0.0f;
    params.outputGain = 0.5f;
    float source[600];
    float leftOut[600]{};
    float rightOut[600]{};
    for (unsigned i = 0; i < 600; ++i) {
        source[i] = 0.25f * std::sin(static_cast<float>(i) * 0.1f);
    }
    audiox::effects::processNoiseReduction(source, leftOut, 257, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &left);
    audiox::effects::processNoiseReduction(source, rightOut, 257, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &right);
    audiox::effects::processNoiseReduction(source + 257, leftOut + 257, 343, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &left);
    audiox::effects::processNoiseReduction(source + 257, rightOut + 257, 343, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &right);
    assert(std::equal(leftOut, leftOut + 600, rightOut));
    assert(audiox::effects::noiseReductionHasPendingAudio(left));

    params.reductionDb = 100.0f;
    params.noiseFloorDb = -120.0f;
    params.adaptation = 2.0f;
    params.smoothing = -1.0f;
    params.mix = 2.0f;
    params.outputGain = 8.0f;
    audiox::effects::processNoiseReduction(source, leftOut, 600, params.reductionDb,
                                           params.noiseFloorDb, params.adaptation, params.smoothing,
                                           params.mix, params.outputGain, &left);
    for (float sample : leftOut) {
        assert(std::isfinite(sample));
    }
}

void testFrozenNoiseEstimate() {
    audiox::effects::NoiseReductionState state{};
    std::vector<float> input(1024), output(input.size());
    audiox::effects::processNoiseReduction(input.data(), output.data(), input.size(), 12, -55, 0,
                                           .7f, 1, 1, &state);
    state.noiseMagnitude.fill(.01f);
    const auto frozen = state.noiseMagnitude;
    audiox::effects::processNoiseReduction(input.data(), output.data(), input.size(), 12, -55, 0,
                                           .7f, 1, 1, &state);
    assert(state.noiseMagnitude == frozen);
}

} // namespace

int main() {
    testEqualizerIdentityAndResponse();
    testEqualizerTransitionsAndPending();
    testNoiseReductionDelayAndPartitioning();
    testNoiseReductionAttenuatesStationaryNoiseAndKeepsTone();
    testNoiseReductionChannelIsolationAndControls();
    testFrozenNoiseEstimate();
    std::puts("audio EQ/noise-reduction tests passed");
    return 0;
}
