#include "audio/effects/dynamics.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace audiox::effects {
namespace {

double control(float value, double lo, double hi, double fallback) {
    return std::isfinite(value) ? std::clamp(static_cast<double>(value), lo, hi) : fallback;
}

double sample(float value) { return std::isfinite(value) ? value : 0.0; }

float output(double value) {
    constexpr double limit = std::numeric_limits<float>::max();
    return std::isfinite(value) ? static_cast<float>(std::clamp(value, -limit, limit)) : 0.0f;
}

double linear(double db) { return std::pow(10.0, db / 20.0); }

double coefficient(double ms) { return std::exp(-1000.0 / (ms * SAMPLE_RATE)); }

double cleanReduction(double value) {
    return std::isfinite(value) ? std::clamp(value, 0.0, 1000.0) : 0.0;
}

double compress(double input, double threshold, double ratio, double knee, double attack,
                double release, double &reduction) {
    if (ratio == 1.0) {
        reduction = 0.0;
        return input;
    }
    const double over = 20.0 * std::log10(std::max(std::abs(input), 1.0e-30)) - threshold;
    double target = 0.0;
    if (over >= knee * 0.5) {
        target = (1.0 - 1.0 / ratio) * over;
    } else if (knee > 0.0 && over > -knee * 0.5) {
        const double position = over + knee * 0.5;
        target = (1.0 - 1.0 / ratio) * position * position / (2.0 * knee);
    }
    const double coef = target > reduction ? attack : release;
    reduction = target + coef * (reduction - target);
    if (reduction < 1.0e-15)
        reduction = 0.0;
    return input * linear(-reduction);
}

struct FilterCoefficients {
    double a1, a2, a3;
    explicit FilterCoefficients(double hz) {
        const double g = std::tan(3.14159265358979323846 * hz / SAMPLE_RATE);
        a1 = 1.0 / (1.0 + g * (g + std::sqrt(2.0)));
        a2 = g * a1;
        a3 = g * a2;
    }
};

struct FilterOutput {
    double low, high, allpass;
};

FilterOutput filter(double input, DynamicsFilterState &state, const FilterCoefficients &c) {
    const double v3 = input - state.integrator2;
    const double band = c.a1 * state.integrator1 + c.a2 * v3;
    const double low = state.integrator2 + c.a2 * state.integrator1 + c.a3 * v3;
    state.integrator1 = 2.0 * band - state.integrator1;
    state.integrator2 = 2.0 * low - state.integrator2;
    if (std::abs(state.integrator1) < 1.0e-30)
        state.integrator1 = 0.0;
    if (std::abs(state.integrator2) < 1.0e-30)
        state.integrator2 = 0.0;
    const double high = input - std::sqrt(2.0) * band - low;
    return {low, high, input - 2.0 * std::sqrt(2.0) * band};
}

double pendingThreshold(float value) {
    return control(value, 0.0, std::numeric_limits<float>::max(), 1.0e-12);
}

} // namespace

#ifndef AUDIOX_DSP_RUNTIME_ONLY
void processCompressor(const float *in, float *out, uint32_t frames, float thresholdDb, float ratio,
                       float attackMs, float releaseMs, float kneeDb, float makeupDb, float mix,
                       CompressorState *state) {
    if (!in || !out || !state || !frames)
        return;
    const double threshold = control(thresholdDb, -60, 0, -18);
    const double r = control(ratio, 1, 20, 4);
    const double attack = coefficient(control(attackMs, 0.1, 100, 10));
    const double release = coefficient(control(releaseMs, 5, 1000, 120));
    const double knee = control(kneeDb, 0, 24, 6);
    const double makeup = linear(control(makeupDb, -12, 24, 0));
    const double wet = control(mix, 0, 1, 1);
    double reduction = cleanReduction(state->reductionDb);
    for (uint32_t i = 0; i < frames; ++i) {
        const double dry = sample(in[i]);
        const double compressed = compress(dry, threshold, r, knee, attack, release, reduction);
        out[i] = output((1.0 - wet) * dry + wet * makeup * compressed);
    }
    state->reductionDb = reduction;
}

void processLimiter(const float *in, float *out, uint32_t frames, float ceilingDb, float releaseMs,
                    float lookaheadMs, float inputGain, LimiterState *state) {
    if (!in || !out || !state || !frames)
        return;
    const double ceilingLinear = linear(control(ceilingDb, -24, 0, -1));
    float ceiling = static_cast<float>(ceilingLinear);
    if (ceiling > ceilingLinear)
        ceiling = std::nextafter(ceiling, 0.0f);
    const double release = coefficient(control(releaseMs, 5, 1000, 80));
    const double inputLevel = control(inputGain, 0, 4, 1);
    const uint32_t delay = std::min(
        LimiterState::kMaxLookaheadFrames,
        static_cast<uint32_t>(std::lround(control(lookaheadMs, 0, 10, 3) * SAMPLE_RATE / 1000.0)));
    if (!state->initialized || state->lookaheadFrames != delay) {
        *state = {};
        state->initialized = true;
        state->lookaheadFrames = delay;
    }
    const uint32_t size = static_cast<uint32_t>(state->delay.size());
    uint32_t write = state->writeIndex % size;
    double gain = std::isfinite(state->gain) ? std::clamp(state->gain, 0.0, 1.0) : 1.0;
    for (uint32_t i = 0; i < frames; ++i) {
        const double current = sample(in[i]) * inputLevel;
        const uint32_t queueSize = static_cast<uint32_t>(state->peaks.size());
        const uint64_t now = state->sampleIndex++;
        while (state->peakHead != state->peakTail &&
               now - state->peakTimes[state->peakHead] > delay)
            state->peakHead = (state->peakHead + 1) % queueSize;
        while (state->peakHead != state->peakTail) {
            const uint32_t previous = (state->peakTail + queueSize - 1) % queueSize;
            if (state->peaks[previous] > std::abs(current))
                break;
            state->peakTail = previous;
        }
        state->peaks[state->peakTail] = std::abs(current);
        state->peakTimes[state->peakTail] = now;
        state->peakTail = (state->peakTail + 1) % queueSize;
        const double level = state->peaks[state->peakHead];
        const double target = level > ceiling ? ceiling / level : 1.0;
        gain = target < gain ? target : target + release * (gain - target);
        state->delay[write] = output(current);
        const uint32_t read = (write + size - delay) % size;
        const double delayed = sample(state->delay[read]);
        // Rounding safety only: the peak window holds gain until stored peaks emerge.
        out[i] = std::clamp(output(delayed * gain), -ceiling, ceiling);
        write = (write + 1) % size;
    }
    state->writeIndex = write;
    state->gain = gain;
}

void processMultibandCompressor(const float *in, float *out, uint32_t frames, float lowHz,
                                float highHz, float lowThresholdDb, float midThresholdDb,
                                float highThresholdDb, float ratio, float releaseMs, float makeupDb,
                                MultibandCompressorState *state) {
    if (!in || !out || !state || !frames)
        return;
    const double maxHz = SAMPLE_RATE * 0.45;
    const double low = std::min(control(lowHz, 80, 1000, 200), maxHz / 1.1);
    const double high = std::max(low * 1.1, std::min(control(highHz, 1000, 10000, 2500), maxHz));
    const FilterCoefficients lowCoeffs(low), highCoeffs(high);
    const double thresholds[] = {control(lowThresholdDb, -60, 0, -18),
                                 control(midThresholdDb, -60, 0, -18),
                                 control(highThresholdDb, -60, 0, -18)};
    const double r = control(ratio, 1, 20, 4);
    const double attack = coefficient(10);
    const double release = coefficient(control(releaseMs, 5, 1000, 120));
    const double makeup = linear(control(makeupDb, -12, 24, 0));
    for (auto &f : state->filters) {
        if (!std::isfinite(f.integrator1) || !std::isfinite(f.integrator2))
            f = {};
    }
    for (auto &reduction : state->reductionDb)
        reduction = cleanReduction(reduction);
    auto &f = state->filters;
    for (uint32_t i = 0; i < frames; ++i) {
        const double input = sample(in[i]);
        const double lowBand = filter(filter(input, f[0], lowCoeffs).low, f[1], lowCoeffs).low;
        const double upper = filter(filter(input, f[2], lowCoeffs).high, f[3], lowCoeffs).high;
        const double bands[] = {
            filter(lowBand, f[8], highCoeffs).allpass,
            filter(filter(upper, f[4], highCoeffs).low, f[5], highCoeffs).low,
            filter(filter(upper, f[6], highCoeffs).high, f[7], highCoeffs).high};
        double sum = 0;
        for (unsigned band = 0; band < 3; ++band) {
            sum += compress(bands[band], thresholds[band], r, 6, attack, release,
                            state->reductionDb[band]);
        }
        out[i] = output(makeup * sum);
    }
}

#endif
void resetCompressor(CompressorState *state) {
    if (state)
        *state = {};
}
void resetLimiter(LimiterState *state) {
    if (state)
        *state = {};
}
void resetMultibandCompressor(MultibandCompressorState *state) {
    if (state)
        *state = {};
}
bool compressorHasPendingAudio(const CompressorState &, float) {
    return false; // Gain memory alone cannot produce audio from silence.
}
bool limiterHasPendingAudio(const LimiterState &state, float threshold) {
    const double level = pendingThreshold(threshold);
    for (float value : state.delay) {
        if (std::isfinite(value) && std::abs(static_cast<double>(value)) > level)
            return true;
    }
    return false;
}
bool multibandCompressorHasPendingAudio(const MultibandCompressorState &state, float threshold) {
    const double level = pendingThreshold(threshold);
    for (const auto &f : state.filters) {
        if ((std::isfinite(f.integrator1) && std::abs(f.integrator1) > level) ||
            (std::isfinite(f.integrator2) && std::abs(f.integrator2) > level))
            return true;
    }
    return false;
}

} // namespace audiox::effects
