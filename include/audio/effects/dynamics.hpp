#pragma once

#include "defs.hpp"

#include <array>
#include <cstdint>

namespace audiox::effects {

struct CompressorState {
    double reductionDb{};
};

struct LimiterState {
    static constexpr uint32_t kMaxLookaheadFrames = (SAMPLE_RATE + 99) / 100;
    std::array<float, kMaxLookaheadFrames + 1> delay{};
    uint32_t writeIndex{};
    double gain{1.0};
    uint32_t lookaheadFrames{};
    bool initialized{};
    std::array<double, kMaxLookaheadFrames + 2> peaks{};
    std::array<uint64_t, kMaxLookaheadFrames + 2> peakTimes{};
    uint32_t peakHead{}, peakTail{};
    uint64_t sampleIndex{};
};

struct DynamicsFilterState {
    double integrator1{};
    double integrator2{};
};

struct MultibandCompressorState {
    std::array<DynamicsFilterState, 9> filters{};
    std::array<double, 3> reductionDb{};
};

// Mono, in-place safe, allocation-free; use a separate state per channel.
// Nonfinite input is silence; nonfinite controls use the documented defaults.
// thresholdDb [-60,0]=-18; ratio [1,20]=4; attackMs [0.1,100]=10;
// releaseMs [5,1000]=120; kneeDb [0,24]=6; makeupDb [-12,24]=0; mix [0,1]=1.
// Unity ratio clears gain reduction immediately. Compressor has no audio tail.
void processCompressor(const float *in, float *out, uint32_t frames, float thresholdDb, float ratio,
                       float attackMs, float releaseMs, float kneeDb, float makeupDb, float mix,
                       CompressorState *state);

// Sample-peak (NOT true-peak) limiter. Latency is round(lookaheadMs * SAMPLE_RATE / 1000).
// The final sample safety clamp also applies immediately after control changes.
// Changing lookahead clears delay history; pending includes the history buffer.
// ceilingDb [-24,0]=-1; releaseMs [5,1000]=80; lookaheadMs [0,10]=3; inputGain [0,4]=1.
void processLimiter(const float *in, float *out, uint32_t frames, float ceilingDb, float releaseMs,
                    float lookaheadMs, float inputGain, LimiterState *state);

// Fourth-order Linkwitz-Riley splits with high-crossover phase compensation on low.
// At ratio=1 and makeup=0 the sum is two cascaded unity-magnitude allpasses, not
// a sample-identical bypass. Attack is 10 ms and knee is 6 dB in each band.
// Cutoffs are bounded to 0.45 * SAMPLE_RATE with high >= 1.1 * low.
// lowHz [80,1000]=200; highHz [1000,10000]=2500; each thresholdDb [-60,0]=-18;
// ratio [1,20]=4; releaseMs [5,1000]=120; makeupDb [-12,24]=0.
void processMultibandCompressor(const float *in, float *out, uint32_t frames, float lowHz,
                                float highHz, float lowThresholdDb, float midThresholdDb,
                                float highThresholdDb, float ratio, float releaseMs, float makeupDb,
                                MultibandCompressorState *state);

void resetCompressor(CompressorState *state);
void resetLimiter(LimiterState *state);
void resetMultibandCompressor(MultibandCompressorState *state);
bool compressorHasPendingAudio(const CompressorState &state, float threshold = 1.0e-12f);
bool limiterHasPendingAudio(const LimiterState &state, float threshold = 1.0e-12f);
bool multibandCompressorHasPendingAudio(const MultibandCompressorState &state,
                                        float threshold = 1.0e-12f);

} // namespace audiox::effects
