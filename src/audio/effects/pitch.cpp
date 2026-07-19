#include "audio/effects/pitch.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace audiox::effects {

namespace {

// Two-grain overlap-add pitch shifter with persistent state.
// Each grain uses a Hann window of period 2*GRAIN; the grains are
// staggered by GRAIN samples so their windows sum to exactly 1.0.
// At each grain boundary (where the window crosses zero), the read
// cursor is snapped to the current write head so the shift stays
// in sync without audible discontinuities.
struct PitchState {
    static constexpr uint32_t BUF_BITS = 13U;
    static constexpr uint32_t BUF_SIZE = 1U << BUF_BITS; // 8192 samples
    static constexpr uint32_t BUF_MASK = BUF_SIZE - 1U;
    static constexpr uint32_t GRAIN    = 512U;            // ~10.7 ms at 48 kHz

    float    buf[BUF_SIZE];
    uint32_t w;      // integer write cursor
    float    r0;     // fractional read cursor, grain 0, in [0, BUF_SIZE)
    float    r1;     // fractional read cursor, grain 1
    uint32_t phase;  // shared grain phase in [0, 2*GRAIN)
};

PitchState *getStateFor(const char *effectId, uint8_t channel) {
    static std::unordered_map<std::string, PitchState> table;
    std::string key = effectId ? effectId : "fx";
    key.push_back('#');
    key += std::to_string((unsigned)channel);
    auto it = table.find(key);
    if (it == table.end()) {
        PitchState &s = table[key];
        memset(&s, 0, sizeof(s));
        s.w     = PitchState::GRAIN * 2U;
        s.r0    = (float)PitchState::GRAIN;
        s.r1    = (float)PitchState::GRAIN;
        s.phase = 0U;
        return &s;
    }
    return &it->second;
}

inline float readBuf(const float *buf, float idx, uint32_t mask) {
    const uint32_t i0  = (uint32_t)idx & mask;
    const uint32_t i1  = (i0 + 1U) & mask;
    const float    frc = idx - floorf(idx);
    return buf[i0] * (1.0f - frc) + buf[i1] * frc;
}

} // namespace

void processPitch(const char *effectId,
                  uint8_t channel,
                  const float *in,
                  float *out,
                  uint32_t frames,
                  float inputGain,
                  float semitoneShift,
                  float mix,
                  float outputGain) {
    if (!in || !out || frames == 0) {
        return;
    }

    if (inputGain  < 0.0f)  inputGain  = 0.0f;
    if (inputGain  > 4.0f)  inputGain  = 4.0f;
    if (semitoneShift < -12.0f) semitoneShift = -12.0f;
    if (semitoneShift >  12.0f) semitoneShift =  12.0f;
    if (mix        < 0.0f)  mix        = 0.0f;
    if (mix        > 1.0f)  mix        = 1.0f;
    if (outputGain < 0.0f)  outputGain = 0.0f;
    if (outputGain > 4.0f)  outputGain = 4.0f;

    PitchState *s = getStateFor(effectId, channel);

    const float rate        = powf(2.0f, semitoneShift / 12.0f);
    const float piOverGrain = (float)M_PI / (float)PitchState::GRAIN;
    const float bufSizeF    = (float)PitchState::BUF_SIZE;

    for (uint32_t i = 0; i < frames; ++i) {
        s->buf[s->w & PitchState::BUF_MASK] = in[i] * inputGain;

        // Complementary Hann windows: w0 + w1 = 1 everywhere.
        const float t      = (float)s->phase * piOverGrain;
        const float cosVal = cosf(t);
        const float w0     = 0.5f * (1.0f - cosVal);
        const float w1     = 0.5f * (1.0f + cosVal);

        const float g0 = readBuf(s->buf, s->r0, PitchState::BUF_MASK);
        const float g1 = readBuf(s->buf, s->r1, PitchState::BUF_MASK);

        const float shifted = g0 * w0 + g1 * w1;
        const float dry     = in[i] * inputGain;
        float       sample  = (dry * (1.0f - mix) + shifted * mix) * outputGain;

        if (sample >  1.0f) sample =  1.0f;
        if (sample < -1.0f) sample = -1.0f;
        out[i] = sample;

        s->w++;
        s->phase++;

        s->r0 += rate;
        if (s->r0 >= bufSizeF) s->r0 -= bufSizeF;
        s->r1 += rate;
        if (s->r1 >= bufSizeF) s->r1 -= bufSizeF;

        // At grain boundaries the window is zero, so snapping the read
        // cursor is inaudible.
        if (s->phase >= PitchState::GRAIN * 2U) {
            s->phase = 0U;
            s->r0 = (float)((s->w - PitchState::GRAIN) & PitchState::BUF_MASK);
        } else if (s->phase == PitchState::GRAIN) {
            s->r1 = (float)((s->w - PitchState::GRAIN) & PitchState::BUF_MASK);
        }
    }
}

} // namespace audiox::effects
