#pragma once

#include "defs.hpp"

#include <cstdint>

namespace audiox::effects {

// Stereo-linked builtins receive both channels through the DSP entry's plan
// pointer; a null plan means a mono node, which is processed as L == R.
struct StereoIo {
    const float *in[2];
    float *out[2];
};

struct CrossfeedState {
    double lowL = 0, lowR = 0, highL = 0, highR = 0, prevL = 0, prevR = 0;
    double a0Lo = 0, b1Lo = 0, a0Hi = 1, a1Hi = 0, b1Hi = 0, gain = 1;
    float fcut = 0, feed = 0;
    bool initialized = false;
};

constexpr uint32_t kMaximizerLookahead = SAMPLE_RATE / 500U > 0 ? SAMPLE_RATE / 500U : 1U;

struct MaximizerState {
    float delay[2][kMaximizerLookahead]{};
    float required[kMaximizerLookahead]{};
    float smoothing[kMaximizerLookahead]{};
    uint64_t minPosition[kMaximizerLookahead]{};
    float minValue[kMaximizerLookahead]{};
    uint64_t sample = 0;
    double smoothingSum = kMaximizerLookahead;
    double envelope = 1;
    double gain = 1;
    uint32_t minHead = 0, minCount = 0, cursor = 0;
    bool initialized = false;
};

void resetCrossfeed(CrossfeedState *state);
bool crossfeedHasPendingAudio(const CrossfeedState &state, float threshold = 1.0e-12f);
// Parameters: fcut_hz, feed_db, output_gain (Bauer stereo-to-binaural, as in libbs2b).
void processCrossfeed(const StereoIo &io, uint32_t frames, const float *params,
                      CrossfeedState *state);

void resetMaximizer(MaximizerState *state);
bool maximizerHasPendingAudio(const MaximizerState &state, float threshold = 1.0e-12f);
// Parameters: threshold_db, ceiling_db, release_ms. Linked lookahead peak limiting
// keeps every output sample at or below the ceiling.
void processMaximizer(const StereoIo &io, uint32_t frames, const float *params,
                      MaximizerState *state);

} // namespace audiox::effects
