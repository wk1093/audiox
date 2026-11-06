#include "audio/effects/pitch.hpp"
#include "defs.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <limits>
#include "audio/effects/fft.hpp"
#include <array>

constexpr double pi = 3.14159265358979323846;

static std::vector<float> render(float shift, float mix, float amplitude, double hz,
                                 uint8_t channel = 0, uint32_t partition = BUFFER_FRAMES, uint8_t quality = 0) {
    audiox::effects::resetPitch("pitch_measure", channel);
    std::vector<float> input(SAMPLE_RATE), output(input.size());
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = amplitude * sin(2 * pi * hz * i / SAMPLE_RATE);
    for (size_t i = 0; i < input.size(); i += partition)
        audiox::effects::processPitch("pitch_measure", channel, input.data() + i, output.data() + i,
                                      std::min<size_t>(partition, input.size() - i), 1, shift, mix,
                                      1, .0001f, 18000, quality);
    return output;
}

static double rms(const std::vector<float> &samples) {
    double power = 0;
    for (size_t i = SAMPLE_RATE / 4; i < samples.size(); ++i) {
        assert(std::isfinite(samples[i]));
        power += samples[i] * samples[i];
    }
    return sqrt(power / (samples.size() - SAMPLE_RATE / 4));
}

static double peakFrequency(const std::vector<float> &samples, double expected) {
    double best = 0, peak = 0;
    const size_t start = SAMPLE_RATE / 2;
    const size_t count = samples.size() - start;
    for (double hz = expected - 16; hz <= expected + 16; hz += .5) {
        double re = 0, im = 0;
        for (size_t i = 0; i < count; ++i) {
            const double window = .5 - .5 * cos(2 * pi * i / count);
            const double angle = 2 * pi * hz * i / SAMPLE_RATE;
            re += samples[start + i] * cos(angle) * window;
            im += samples[start + i] * sin(angle) * window;
        }
        const double power = re * re + im * im;
        if (power > peak) {
            peak = power;
            best = hz;
        }
    }
    return best;
}

int main() {
    const audiox::effects::FftPlan<1024> plan;
    std::array<float, 2048> original{}, planned{};
    for (size_t i = 0; i < original.size(); ++i) original[i] = sin(i * .731) * .1;
    for (bool inverse : {false, true}) {
        planned = original;
        auto reference = original;
        audiox::effects::fftComplex(reference.data(), 1024, inverse);
        plan.transform(planned.data(), inverse);
        for (size_t i = 0; i < planned.size(); ++i) assert(fabs(planned[i] - reference[i]) < 1e-6f);
    }
    for (float shift : {-12.f, -7.f, 0.f, 7.f, 12.f}) {
        const double expected = 440 * pow(2, shift / 12.);
        const auto samples = render(shift, 1, .1f, 440);
        const double hz = peakFrequency(samples, expected);
        printf("Pitch %+g st: expected %.2f Hz, measured %.2f Hz, RMS %.5f (input %.5f)\n", shift,
               expected, hz, rms(samples), .1 / sqrt(2));
        assert(fabs(hz - expected) < 2);
        assert(rms(samples) > .035 && rms(samples) < .105);
    }
    const auto unity = render(0, 1, .2f, 1000);
    for (size_t i = 0; i < unity.size(); ++i)
        assert(fabs(unity[i] - .2 * sin(2 * pi * 1000 * i / SAMPLE_RATE)) < 1e-6);
    const auto wet = render(7, 1, .1f, 440);
    const auto partitioned = render(7, 1, .1f, 440, 0, 73);
    for (size_t i = 0; i < wet.size(); ++i)
        assert(fabs(wet[i] - partitioned[i]) < 1e-6);
    const auto mixed = render(7, .5f, .1f, 440);
    for (size_t i = 0; i < mixed.size(); ++i) {
        const float delayedDry = i < 1024 ? 0 : .1 * sin(2 * pi * 440 * (i - 1024) / SAMPLE_RATE);
        assert(fabs(mixed[i] - (wet[i] + delayedDry) * .5f) < 1e-6);
    }
    const auto isolated = render(7, 1, .1f, 440, 1);
    assert(isolated == wet);
    for (uint8_t quality : {1, 2}) {
        for (float shift : {-12.f, -7.f, 7.f, 12.f}) {
            const double expected = 440 * pow(2, shift / 12.);
            const auto shifted = render(shift, 1, .1f, 440, 0, BUFFER_FRAMES, quality);
            const double hz = peakFrequency(shifted, expected);
            printf("Pitch quality %u %+g st: %.2f Hz, RMS %.5f\n", quality, shift, hz, rms(shifted));
            assert(fabs(hz - expected) < 2);
            assert(rms(shifted) > .015 && rms(shifted) < .13);
        }
        const auto shifted = render(7, 1, .1f, 440, 0, BUFFER_FRAMES, quality);
        const auto chunks = render(7, 1, .1f, 440, 0, 73, quality);
        assert(shifted == chunks);
        const auto mix = render(7, .5, .1f, 440, 0, BUFFER_FRAMES, quality);
        for (size_t i = 0; i < mix.size(); ++i) {
            const float dry = i < 1024 ? 0 : .1 * sin(2 * pi * 440 * (i - 1024) / SAMPLE_RATE);
            assert(fabs(mix[i] - (shifted[i] + dry) * .5f) < 1e-6);
        }
        for (double frequency : {110., 1000., 4000.}) {
            const auto tone = render(7, 1, .1f, frequency, 0, BUFFER_FRAMES, quality);
            const double expected = frequency * pow(2, 7 / 12.);
            assert(fabs(peakFrequency(tone, expected) - expected) < 2);
            assert(rms(tone) > .005 && rms(tone) < .15);
        }
    }
    std::array<float, 2048> transitionInput{}, transitionOutput{};
    for (size_t i = 0; i < transitionInput.size(); ++i) transitionInput[i] = .1f * sin(i * .04);
    for (uint8_t quality : {0, 1, 2, 0}) {
        audiox::effects::processPitch("transition", 0, transitionInput.data(), transitionOutput.data(),
                                      transitionInput.size(), 1, 7, 1, 1, .001, 12000, quality);
        audiox::effects::resetPitch("fresh", 0);
        std::array<float, 2048> fresh{};
        audiox::effects::processPitch("fresh", 0, transitionInput.data(), fresh.data(),
                                      fresh.size(), 1, 7, 1, 1, .001, 12000, quality);
        assert(transitionOutput == fresh);
    }
    audiox::effects::resetPitch("pitch_measure", 0);
    assert(!audiox::effects::pitchHasPendingAudio("pitch_measure", 0, 1e-12f));
    std::vector<float> silence(4096), output(silence.size());
    audiox::effects::processPitch("pitch_measure", 0, silence.data(), output.data(), output.size(),
                                  1, 7, 1, 1, .0001f, 18000);
    for (float sample : output)
        assert(sample == 0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float source[]{.1f, -.2f, 0};
    float finite[3]{};
    audiox::effects::processPitch("finite_pitch_controls", 0, source, finite, 3, nan, nan, nan, nan,
                                  nan, nan);
    for (size_t i = 0; i < 3; ++i)
        assert(source[i] == finite[i]);
    puts("Pitch frequency, normalized level, unity, partitioning, 1024-frame aligned mix, "
         "isolation and reset tests passed");
}
