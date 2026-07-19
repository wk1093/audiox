#include "audio/effects/pitch.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

namespace audiox::effects {

namespace {

// Single-grain pitch shifter with aperiodic crossfade re-sync.
//
// A circular buffer holds recent input. One read cursor advances at
// `rate` samples per output sample while the write cursor advances at 1.
// When the read cursor drifts outside the safe lag window a short
// smoothstep crossfade blends to a new cursor at the target lag.
// Crossfades occur only when the constraint is violated, not on a fixed
// schedule, so they produce no periodic amplitude modulation (tremolo).
struct PitchState {
    static constexpr uint32_t BUF_BITS   = 14U;
    static constexpr uint32_t BUF_SIZE   = 1U << BUF_BITS; // 16384 samples
    static constexpr uint32_t BUF_MASK   = BUF_SIZE - 1U;
    static constexpr uint32_t FADE_LEN   = 1024U;           // ~21 ms at 48 kHz
    static constexpr uint32_t TARGET_LAG = BUF_SIZE / 4U;  // 4096 samples
    static constexpr uint32_t MIN_LAG    = FADE_LEN * 2U;  // 2048
    static constexpr uint32_t MAX_LAG    = BUF_SIZE - FADE_LEN * 2U; // 14336

    float    buf[BUF_SIZE];
    uint32_t w;        // write cursor
    float    r0;       // primary read cursor in [0, BUF_SIZE)
    float    r1;       // crossfade target cursor
    uint32_t fadePos;  // 0 = idle; 1..FADE_LEN = crossfade active
    bool     inited;
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

    if (!s->inited) {
        memset(s->buf, 0, sizeof(s->buf));
        s->w       = PitchState::TARGET_LAG * 2U;
        s->r0      = (float)PitchState::TARGET_LAG;
        s->r1      = s->r0;
        s->fadePos = 0U;
        s->inited  = true;
    }

    const float rate    = powf(2.0f, semitoneShift / 12.0f);
    const float bufF    = (float)PitchState::BUF_SIZE;
    const float minLagF = (float)PitchState::MIN_LAG;
    const float maxLagF = (float)PitchState::MAX_LAG;

    for (uint32_t i = 0; i < frames; ++i) {
        s->buf[s->w & PitchState::BUF_MASK] = in[i] * inputGain;

        float g;
        if (s->fadePos > 0U) {
            // Smoothstep crossfade from r0 (old) to r1 (new)
            const float t     = (float)s->fadePos / (float)PitchState::FADE_LEN;
            const float alpha = t * t * (3.0f - 2.0f * t);
            const float g0    = readBuf(s->buf, s->r0, PitchState::BUF_MASK);
            const float g1    = readBuf(s->buf, s->r1, PitchState::BUF_MASK);
            g = g0 * (1.0f - alpha) + g1 * alpha;

            s->r0 += rate; if (s->r0 >= bufF) s->r0 -= bufF;
            s->r1 += rate; if (s->r1 >= bufF) s->r1 -= bufF;
            s->fadePos++;
            if (s->fadePos >= PitchState::FADE_LEN) {
                s->r0      = s->r1;
                s->fadePos = 0U;
            }
        } else {
            g = readBuf(s->buf, s->r0, PitchState::BUF_MASK);
            s->r0 += rate;
            if (s->r0 >= bufF) s->r0 -= bufF;
        }

        const float dry    = in[i] * inputGain;
        float       sample = (dry * (1.0f - mix) + g * mix) * outputGain;
        if (sample >  1.0f) sample =  1.0f;
        if (sample < -1.0f) sample = -1.0f;
        out[i] = sample;

        s->w++;

        // Trigger crossfade only when lag drifts outside the safe window.
        if (s->fadePos == 0U) {
            const float wpos = (float)(s->w & PitchState::BUF_MASK);
            float       lag  = wpos - s->r0;
            if (lag < 0.0f) lag += bufF;

            if (lag < minLagF || lag > maxLagF) {
                float new_r1 = wpos - (float)PitchState::TARGET_LAG;
                if (new_r1 < 0.0f) new_r1 += bufF;
                s->r1      = new_r1;
                s->fadePos = 1U;
            }
        }
    }
}

} // namespace audiox::effects
