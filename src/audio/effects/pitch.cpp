#include "audio/effects/pitch.hpp"
#include "defs.hpp"
#include "audio/effects/state_bank.hpp"
#include "audio/effects/fft.hpp"

#include <cmath>
#include <cstring>

#define AUDIOX_PITCH_VOCODER_MAX_SEMITONES 12.0f

namespace audiox::effects {

static_assert((PitchState::FFT_SIZE & (PitchState::FFT_SIZE - 1U)) == 0U,
              "AUDIOX_PITCH_VOCODER_FFT_SIZE must be a power of two");

constexpr float kPi = 3.14159265358979323846f;

#ifndef AUDIOX_DSP_BITCODE
PitchPlan::PitchPlan() {
    for (uint32_t i = 0; i < PitchState::FFT_SIZE; ++i) {
        window[i] = .5f - .5f * cosf(2 * kPi * i / PitchState::FFT_SIZE);
    }
    for (uint32_t quality = 0; quality < 3; ++quality) {
        const uint32_t overlap = 8U >> quality;
        for (uint32_t i = 0; i < PitchState::FFT_SIZE; ++i) {
            const float scale = 8.0f / (3.0f * PitchState::FFT_SIZE * overlap);
            synthesis[quality][i] = window[i] * scale;
        }
    }
}
namespace { const PitchPlan pitchPlan; }
const PitchPlan &getPitchPlan() { return pitchPlan; }
StateBank<PitchState> pitchStates;
PitchState *pitchStateFor(const char *effectId, uint8_t channel, bool create) {
    return pitchStates.find(effectId, channel, create);
}
#endif

#ifndef AUDIOX_DSP_RUNTIME_ONLY
namespace {
inline void processFrame(PitchState *s, const PitchPlan &pitchPlan, float pitchRatio, float noiseFloorRatio) {
    const uint32_t fftSize = PitchState::FFT_SIZE;
    const uint32_t hop = fftSize / (8U >> s->quality);
    const uint32_t halfBins = PitchState::HALF_BINS;
    const float osampF = (float)(8U >> s->quality);
    const float expectedPhaseAdvance = 2.0f * kPi * (float)hop / (float)fftSize;

    for (uint32_t i = 0U; i < fftSize; ++i) {
        s->fftWork[i * 2U] = s->inFifo[i] * pitchPlan.window[i];
        s->fftWork[i * 2U + 1U] = 0.0f;
    }

    pitchPlan.fft.transform(s->fftWork, false);

    float maxAnaMag = 0.0f;

    for (uint32_t k = 0U; k < halfBins; ++k) {
        const float real = s->fftWork[k * 2U];
        const float imag = s->fftWork[k * 2U + 1U];
        const float mag = sqrtf(real * real + imag * imag);
        const float phase = atan2f(imag, real);

        float delta = phase - s->lastPhase[k];
        s->lastPhase[k] = phase;
        delta -= (float)k * expectedPhaseAdvance;

        int qpd = (int)(delta / kPi);
        if (qpd >= 0) {
            qpd += qpd & 1;
        } else {
            qpd -= qpd & 1;
        }
        delta -= kPi * (float)qpd;

        const float trueBin = (float)k + (osampF * delta / (2.0f * kPi));
        s->anaMag[k] = mag;
        s->anaFreq[k] = trueBin;
        if (mag > maxAnaMag) {
            maxAnaMag = mag;
        }
    }

    const float noiseFloor = maxAnaMag * noiseFloorRatio;

    memset(s->synMag, 0, sizeof(s->synMag));
    memset(s->synFreqAcc, 0, sizeof(s->synFreqAcc));
    for (uint32_t k = 0U; k < halfBins; ++k) {
        if (s->anaMag[k] < noiseFloor)
            continue;
        const uint32_t bin = static_cast<uint32_t>(k * pitchRatio);
        if (bin >= halfBins)
            continue;
        uint32_t frequencyBin = k;
        // At 2x overlap phase unwrapping cannot resolve the Hann main lobe's
        // outer bins. Use the local spectral peak's frequency for those bins.
        if (s->quality == 2) {
            const uint32_t first = k > 2 ? k - 2 : 0;
            const uint32_t last = std::min(k + 2, halfBins - 1);
            for (uint32_t neighbor = first; neighbor <= last; ++neighbor)
                if (s->anaMag[neighbor] > s->anaMag[frequencyBin]) frequencyBin = neighbor;
        }
        s->synMag[bin] += s->anaMag[k];
        s->synFreqAcc[bin] += s->anaFreq[frequencyBin] * pitchRatio * s->anaMag[k];
    }
    for (uint32_t k = 0; k < halfBins; ++k) {
        const float frequency = s->synMag[k] > 0 ? s->synFreqAcc[k] / s->synMag[k] : k;
        s->sumPhase[k] = remainderf(s->sumPhase[k] + frequency * expectedPhaseAdvance, 2 * kPi);
        if (s->synMag[k] == 0) {
            s->fftWork[k * 2] = s->fftWork[k * 2 + 1] = 0;
        } else {
            float sine, cosine;
            sincosf(s->sumPhase[k], &sine, &cosine);
            s->fftWork[k * 2] = s->synMag[k] * cosine;
            s->fftWork[k * 2 + 1] = s->synMag[k] * sine;
        }
    }

    s->fftWork[1U] = 0.0f;
    s->fftWork[fftSize] = 0.0f;
    s->fftWork[fftSize + 1U] = 0.0f;

    for (uint32_t k = 1U; k < (fftSize / 2U); ++k) {
        const uint32_t mirror = fftSize - k;
        s->fftWork[mirror * 2U] = s->fftWork[k * 2U];
        s->fftWork[mirror * 2U + 1U] = -s->fftWork[k * 2U + 1U];
    }

    pitchPlan.fft.transform(s->fftWork, true);

    for (uint32_t i = 0U; i < fftSize; ++i) {
        s->outputAccum[i] += pitchPlan.synthesis[s->quality][i] * s->fftWork[i * 2U];
    }

    for (uint32_t i = 0U; i < hop; ++i) {
        s->outFifo[i] = s->outputAccum[i];
    }

    const uint32_t remain = fftSize - hop;
    memmove(s->outputAccum, s->outputAccum + hop, remain * sizeof(float));
    memset(s->outputAccum + remain, 0, hop * sizeof(float));

    memmove(s->inFifo, s->inFifo + hop, remain * sizeof(float));
}

} // namespace
#endif

#ifndef AUDIOX_DSP_BITCODE
bool pitchHasPendingAudio(const char *effectId, uint8_t channel, float threshold) {
    const PitchState *state = pitchStateFor(effectId, channel, false);
    if (!state)
        return false;
    for (uint32_t i = 0; i < PitchState::FFT_SIZE; ++i) {
        if (!(fabsf(state->inFifo[i]) <= threshold) || !(fabsf(state->outFifo[i]) <= threshold) ||
            !(fabsf(state->dryDelay[i]) <= threshold) ||
            !(fabsf(state->outputAccum[i]) <= threshold))
            return true;
    }
    return !(fabsf(state->wetLpfZ) <= threshold);
}

void resetPitch(const char *effectId, uint8_t channel) {
    PitchState *state = pitchStateFor(effectId, channel, false);
    if (state)
        *state = {};
}
#endif

#ifndef AUDIOX_DSP_RUNTIME_ONLY
#ifndef AUDIOX_DSP_BITCODE
void processPitch(const char *effectId, uint8_t channel, const float *in, float *out,
                  uint32_t frames, float inputGain, float semitoneShift, float mix,
                  float outputGain, float noiseFloorRatio, float wetLpfHz, uint8_t quality) {
    processPitchState(pitchStateFor(effectId, channel), getPitchPlan(), in, out, frames,
                      inputGain, semitoneShift, mix, outputGain, noiseFloorRatio, wetLpfHz, quality);
}
#endif
void processPitchState(PitchState *s, const PitchPlan &plan, const float *in, float *out,
                  uint32_t frames, float inputGain, float semitoneShift, float mix,
                  float outputGain, float noiseFloorRatio, float wetLpfHz, uint8_t quality) {
    if (!s || !in || !out || frames == 0) {
        return;
    }

    if (!std::isfinite(inputGain))
        inputGain = 1;
    if (!std::isfinite(semitoneShift))
        semitoneShift = 0;
    if (!std::isfinite(mix))
        mix = 1;
    if (!std::isfinite(outputGain))
        outputGain = 1;
    if (!std::isfinite(noiseFloorRatio))
        noiseFloorRatio = .001f;
    if (!std::isfinite(wetLpfHz))
        wetLpfHz = 12000;
    if (inputGain < 0.0f)
        inputGain = 0.0f;
    if (inputGain > 4.0f)
        inputGain = 4.0f;
    if (semitoneShift < -AUDIOX_PITCH_VOCODER_MAX_SEMITONES)
        semitoneShift = -AUDIOX_PITCH_VOCODER_MAX_SEMITONES;
    if (semitoneShift > AUDIOX_PITCH_VOCODER_MAX_SEMITONES)
        semitoneShift = AUDIOX_PITCH_VOCODER_MAX_SEMITONES;
    if (mix < 0.0f)
        mix = 0.0f;
    if (mix > 1.0f)
        mix = 1.0f;
    if (outputGain < 0.0f)
        outputGain = 0.0f;
    if (outputGain > 4.0f)
        outputGain = 4.0f;
    if (noiseFloorRatio < 0.0001f)
        noiseFloorRatio = 0.0001f;
    if (noiseFloorRatio > 0.02f)
        noiseFloorRatio = 0.02f;
    if (wetLpfHz < 1000.0f)
        wetLpfHz = 1000.0f;
    if (wetLpfHz > 18000.0f)
        wetLpfHz = 18000.0f;

    if (quality > 2) quality = 0;
    const uint32_t hop = PitchState::FFT_SIZE / (8U >> quality);

    if (fabsf(semitoneShift) < 1.0e-6f || mix == 0.0f) {
        *s = {};
        for (uint32_t i = 0; i < frames; ++i)
            out[i] = std::isfinite(in[i])
                         ? fmaxf(-1.0f, fminf(1.0f, in[i] * inputGain * outputGain))
                         : 0.0f;
        return;
    }
    if (!s->inited || s->quality != quality) {
        *s = {};
        s->quality = quality;
        s->cachedShift = s->cachedLpfHz = NAN;
        s->rover = PitchState::FFT_SIZE - hop;
        s->inited = true;
    }

    if (s->cachedShift != semitoneShift) {
        s->cachedShift = semitoneShift;
        s->pitchRatio = powf(2.0f, semitoneShift / 12.0f);
    }
    if (s->cachedLpfHz != wetLpfHz) {
        s->cachedLpfHz = wetLpfHz;
        s->wetLpfAlpha = expf(-2.0f * kPi * wetLpfHz / (float)SAMPLE_RATE);
    }
    const float pitchRatio = s->pitchRatio;
    const uint32_t inFifoLatency = PitchState::FFT_SIZE - hop;
    const float wetLpfAlpha = s->wetLpfAlpha;

    for (uint32_t i = 0; i < frames; ++i) {
        const float input = std::isfinite(in[i]) ? in[i] * inputGain : 0.0f;
        s->inFifo[s->rover] = input;

        const uint32_t outIndex = s->rover - inFifoLatency;
        const float wetRaw = s->outFifo[outIndex];
        const float wet = (1.0f - wetLpfAlpha) * wetRaw + wetLpfAlpha * s->wetLpfZ;
        s->wetLpfZ = wet;
        const float dry = s->dryDelay[s->dryIndex];
        s->dryDelay[s->dryIndex] = input;
        s->dryIndex = (s->dryIndex + 1U) % PitchState::FFT_SIZE;

        float sample = (dry * (1.0f - mix) + wet * mix) * outputGain;
        if (sample > 1.0f)
            sample = 1.0f;
        if (sample < -1.0f)
            sample = -1.0f;
        out[i] = sample;

        s->rover++;
        if (s->rover >= PitchState::FFT_SIZE) {
            processFrame(s, plan, pitchRatio, noiseFloorRatio);
            s->rover = inFifoLatency;
        }
    }
}
#endif
} // namespace audiox::effects
