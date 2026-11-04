#include "audio/effects/pitch.hpp"
#include "defs.hpp"
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>
#include <limits>

constexpr double pi = 3.14159265358979323846;

static std::vector<float> render(float shift, float mix, float amplitude, double hz,
                                 uint8_t channel = 0, uint32_t partition = BUFFER_FRAMES) {
    audiox::effects::resetPitch("pitch_measure", channel);
    std::vector<float> input(SAMPLE_RATE), output(input.size());
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = amplitude * sin(2 * pi * hz * i / SAMPLE_RATE);
    for (size_t i = 0; i < input.size(); i += partition)
        audiox::effects::processPitch("pitch_measure", channel, input.data() + i, output.data() + i,
                                      std::min<size_t>(partition, input.size() - i), 1, shift, mix,
                                      1, .0001f, 18000);
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
