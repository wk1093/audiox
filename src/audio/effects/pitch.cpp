#include "audio/effects/pitch.hpp"
#include "defs.hpp"
#include "audio/effects/state_bank.hpp"
#include "audio/effects/fft.hpp"

#include <cmath>
#include <cstring>

#define AUDIOX_PITCH_VOCODER_FFT_SIZE 1024U
#define AUDIOX_PITCH_VOCODER_OVERLAP_NUM 7U
#define AUDIOX_PITCH_VOCODER_OVERLAP_DEN 8U
#define AUDIOX_PITCH_VOCODER_HOP_SIZE                                                              \
    ((AUDIOX_PITCH_VOCODER_FFT_SIZE *                                                              \
      (AUDIOX_PITCH_VOCODER_OVERLAP_DEN - AUDIOX_PITCH_VOCODER_OVERLAP_NUM)) /                     \
     AUDIOX_PITCH_VOCODER_OVERLAP_DEN)
#define AUDIOX_PITCH_VOCODER_MAX_SEMITONES 12.0f

namespace audiox::effects {

namespace {

struct PitchState {
    static constexpr uint32_t FFT_SIZE = AUDIOX_PITCH_VOCODER_FFT_SIZE;
    static constexpr uint32_t HOP_SIZE = AUDIOX_PITCH_VOCODER_HOP_SIZE;
    static constexpr uint32_t OSAMP = FFT_SIZE / HOP_SIZE;
    static constexpr uint32_t HALF_BINS = FFT_SIZE / 2U + 1U;

    float inFifo[FFT_SIZE];
    float outFifo[FFT_SIZE];
    float fftWork[FFT_SIZE * 2U];
    float window[FFT_SIZE];
    float outputAccum[FFT_SIZE];
    float dryDelay[FFT_SIZE];
    uint32_t dryIndex;

    float lastPhase[HALF_BINS];
    float sumPhase[HALF_BINS];
    float anaMag[HALF_BINS];
    float anaFreq[HALF_BINS];
    float synMag[HALF_BINS];
    float synFreqAcc[HALF_BINS];

    uint32_t rover;
    float wetLpfZ;
    bool inited;
};

static_assert((PitchState::FFT_SIZE & (PitchState::FFT_SIZE - 1U)) == 0U,
              "AUDIOX_PITCH_VOCODER_FFT_SIZE must be a power of two");
static_assert(PitchState::HOP_SIZE > 0U, "AUDIOX_PITCH_VOCODER_HOP_SIZE must be > 0");
static_assert(PitchState::FFT_SIZE % PitchState::HOP_SIZE == 0U,
              "AUDIOX_PITCH_VOCODER_HOP_SIZE must divide AUDIOX_PITCH_VOCODER_FFT_SIZE");

constexpr float kPi = 3.14159265358979323846f;

inline void processFrame(PitchState *s, float pitchRatio, float noiseFloorRatio) {
    const uint32_t fftSize = PitchState::FFT_SIZE;
    const uint32_t hop = PitchState::HOP_SIZE;
    const uint32_t halfBins = PitchState::HALF_BINS;
    const float osampF = (float)PitchState::OSAMP;
    const float expectedPhaseAdvance = 2.0f * kPi * (float)hop / (float)fftSize;

    for (uint32_t i = 0U; i < fftSize; ++i) {
        s->fftWork[i * 2U] = s->inFifo[i] * s->window[i];
        s->fftWork[i * 2U + 1U] = 0.0f;
    }

    fftComplex(s->fftWork, fftSize, false);

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
        s->synMag[bin] += s->anaMag[k];
        s->synFreqAcc[bin] += s->anaFreq[k] * pitchRatio * s->anaMag[k];
    }
    for (uint32_t k = 0; k < halfBins; ++k) {
        const float frequency = s->synMag[k] > 0 ? s->synFreqAcc[k] / s->synMag[k] : k;
        s->sumPhase[k] = remainderf(s->sumPhase[k] + frequency * expectedPhaseAdvance, 2 * kPi);
        s->fftWork[k * 2] = s->synMag[k] * cosf(s->sumPhase[k]);
        s->fftWork[k * 2 + 1] = s->synMag[k] * sinf(s->sumPhase[k]);
    }

    s->fftWork[1U] = 0.0f;
    s->fftWork[fftSize] = 0.0f;
    s->fftWork[fftSize + 1U] = 0.0f;

    for (uint32_t k = 1U; k < (fftSize / 2U); ++k) {
        const uint32_t mirror = fftSize - k;
        s->fftWork[mirror * 2U] = s->fftWork[k * 2U];
        s->fftWork[mirror * 2U + 1U] = -s->fftWork[k * 2U + 1U];
    }

    fftComplex(s->fftWork, fftSize, true);

    // Hann analysis/synthesis windows sum to 3 * overlap / 8.
    const float scale = 8.0f / (3.0f * (float)fftSize * osampF);
    for (uint32_t i = 0U; i < fftSize; ++i) {
        s->outputAccum[i] += s->window[i] * s->fftWork[i * 2U] * scale;
    }

    for (uint32_t i = 0U; i < hop; ++i) {
        s->outFifo[i] = s->outputAccum[i];
    }

    const uint32_t remain = fftSize - hop;
    memmove(s->outputAccum, s->outputAccum + hop, remain * sizeof(float));
    memset(s->outputAccum + remain, 0, hop * sizeof(float));

    memmove(s->inFifo, s->inFifo + hop, remain * sizeof(float));
}

StateBank<PitchState> pitchStates;
PitchState *getStateFor(const char *effectId, uint8_t channel, bool create = true) {
    return pitchStates.find(effectId, channel, create);
}

} // namespace

bool pitchHasPendingAudio(const char *effectId, uint8_t channel, float threshold) {
    const PitchState *state = getStateFor(effectId, channel, false);
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
    PitchState *state = getStateFor(effectId, channel, false);
    if (state)
        memset(state, 0, sizeof(*state));
}

void processPitch(const char *effectId, uint8_t channel, const float *in, float *out,
                  uint32_t frames, float inputGain, float semitoneShift, float mix,
                  float outputGain, float noiseFloorRatio, float wetLpfHz) {
    if (!in || !out || frames == 0) {
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

    PitchState *s = getStateFor(effectId, channel);

    if (fabsf(semitoneShift) < 1.0e-6f || mix == 0.0f) {
        memset(s, 0, sizeof(*s));
        for (uint32_t i = 0; i < frames; ++i)
            out[i] = std::isfinite(in[i])
                         ? fmaxf(-1.0f, fminf(1.0f, in[i] * inputGain * outputGain))
                         : 0.0f;
        return;
    }
    if (!s->inited) {
        memset(s, 0, sizeof(*s));
        for (uint32_t i = 0U; i < PitchState::FFT_SIZE; ++i) {
            s->window[i] =
                0.5f - 0.5f * cosf((2.0f * kPi * (float)i) / (float)PitchState::FFT_SIZE);
        }
        s->rover = PitchState::FFT_SIZE - PitchState::HOP_SIZE;
        s->inited = true;
    }

    const float pitchRatio = powf(2.0f, semitoneShift / 12.0f);
    const uint32_t inFifoLatency = PitchState::FFT_SIZE - PitchState::HOP_SIZE;
    const float wetLpfAlpha = expf(-2.0f * kPi * wetLpfHz / (float)SAMPLE_RATE);

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
            processFrame(s, pitchRatio, noiseFloorRatio);
            s->rover = inFifoLatency;
        }
    }
}

} // namespace audiox::effects
