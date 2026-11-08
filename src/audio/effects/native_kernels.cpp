#include "audio/effects/native.hpp"
#include "audio/effects/gain.hpp"
#include "audio/effects/distortion.hpp"
#include "audio/effects/pitch.hpp"
#include "audio/effects/reverb.hpp"
#include "audio/effects/gate.hpp"
#include "audio/effects/dynamics.hpp"
#include "audio/effects/equalizer.hpp"
#include "audio/effects/noise_reduction.hpp"
#include <cstring>
#include "defs.hpp"

using namespace audiox::effects;
extern "C" const uint32_t audiox_dsp_sample_rate = SAMPLE_RATE;
extern "C" const uint32_t audiox_dsp_block_frames = BUFFER_FRAMES;

#define KERNEL(n) extern "C" void audiox_dsp_##n( \
    const float *in, float *out, uint32_t frames, const float *p, \
    [[maybe_unused]] void *state, [[maybe_unused]] const void *plan)

KERNEL(0) { processGain(in, out, frames, p[0]); }
KERNEL(1) { processDistortion(in, out, frames, p[0], p[1], p[2], p[3]); }
KERNEL(2) {
    processPitchState(static_cast<PitchState *>(state), *static_cast<const PitchPlan *>(plan),
                      in, out, frames, p[0], p[1], p[2], p[3], p[4], p[5], static_cast<uint8_t>(p[6]));
}
KERNEL(3) { processReverb(in, out, frames, p[0], p[1], p[2], p[3], static_cast<ReverbState *>(state)); }
KERNEL(4) { processGate(in, out, frames, p[0], p[1], p[2], p[3], static_cast<GateState *>(state)); }
extern "C" void audiox_dsp_5(const float *, float *out, uint32_t frames, const float *, void *, const void *) {
    memset(out, 0, sizeof(float) * frames);
}
KERNEL(6) {
    processCompressor(in, out, frames, p[0], p[1], p[2], p[3], p[4], p[5], p[6],
                      static_cast<CompressorState *>(state));
}
KERNEL(7) { processLimiter(in, out, frames, p[0], p[1], p[2], p[3], static_cast<LimiterState *>(state)); }
KERNEL(8) {
    processEqualizer(in, out, frames, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                     static_cast<EqualizerState *>(state));
}
KERNEL(9) {
    processMultibandCompressor(in, out, frames, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7],
                              static_cast<MultibandCompressorState *>(state));
}
KERNEL(10) {
    processNoiseReduction(in, out, frames, p[0], p[1], p[2], p[3], p[4], p[5],
                          static_cast<NoiseReductionState *>(state));
}
#undef KERNEL
