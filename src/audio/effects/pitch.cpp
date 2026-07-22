#include "audio/effects/pitch.hpp"
#include "defs.hpp"

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>

#define AUDIOX_PITCH_VOCODER_FFT_SIZE (BUFFER_FRAMES * 4U)
#define AUDIOX_PITCH_VOCODER_OVERLAP_NUM 3U
#define AUDIOX_PITCH_VOCODER_OVERLAP_DEN 4U
#define AUDIOX_PITCH_VOCODER_HOP_SIZE \
    ((AUDIOX_PITCH_VOCODER_FFT_SIZE * (AUDIOX_PITCH_VOCODER_OVERLAP_DEN - AUDIOX_PITCH_VOCODER_OVERLAP_NUM)) / AUDIOX_PITCH_VOCODER_OVERLAP_DEN)
#define AUDIOX_PITCH_VOCODER_MAX_SEMITONES 12.0f
#define AUDIOX_PITCH_VOCODER_NOISE_FLOOR_RATIO 0.0015f
#define AUDIOX_PITCH_VOCODER_WET_LPF_HZ 9000.0f

namespace audiox::effects {

namespace {

struct PitchState {
    static constexpr uint32_t FFT_SIZE = AUDIOX_PITCH_VOCODER_FFT_SIZE;
    static constexpr uint32_t HOP_SIZE = AUDIOX_PITCH_VOCODER_HOP_SIZE;
    static constexpr uint32_t OSAMP    = FFT_SIZE / HOP_SIZE;
    static constexpr uint32_t HALF_BINS = FFT_SIZE / 2U + 1U;

    float inFifo[FFT_SIZE];
    float outFifo[FFT_SIZE];
    float fftWork[FFT_SIZE * 2U];
    float window[FFT_SIZE];
    float outputAccum[FFT_SIZE];

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

inline void fftComplex(float *data, uint32_t n, bool inverse) {
    uint32_t j = 0U;
    for (uint32_t i = 0U; i < n - 1U; ++i) {
        if (i < j) {
            const float tr = data[i * 2U];
            const float ti = data[i * 2U + 1U];
            data[i * 2U] = data[j * 2U];
            data[i * 2U + 1U] = data[j * 2U + 1U];
            data[j * 2U] = tr;
            data[j * 2U + 1U] = ti;
        }

        uint32_t m = n >> 1U;
        while (m > 0U && j >= m) {
            j -= m;
            m >>= 1U;
        }
        j += m;
    }

    for (uint32_t len = 2U; len <= n; len <<= 1U) {
        const float sign = inverse ? 1.0f : -1.0f;
        const float ang = sign * 2.0f * kPi / (float)len;
        const float wLenR = cosf(ang);
        const float wLenI = sinf(ang);

        for (uint32_t i = 0U; i < n; i += len) {
            float wR = 1.0f;
            float wI = 0.0f;
            const uint32_t half = len >> 1U;

            for (uint32_t k = 0U; k < half; ++k) {
                const uint32_t u = i + k;
                const uint32_t v = u + half;

                const float vR = data[v * 2U] * wR - data[v * 2U + 1U] * wI;
                const float vI = data[v * 2U] * wI + data[v * 2U + 1U] * wR;

                const float uR = data[u * 2U];
                const float uI = data[u * 2U + 1U];

                data[u * 2U] = uR + vR;
                data[u * 2U + 1U] = uI + vI;
                data[v * 2U] = uR - vR;
                data[v * 2U + 1U] = uI - vI;

                const float nextWR = wR * wLenR - wI * wLenI;
                const float nextWI = wR * wLenI + wI * wLenR;
                wR = nextWR;
                wI = nextWI;
            }
        }
    }
}

inline void processFrame(PitchState *s, float pitchRatio) {
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
        const float mag = 2.0f * sqrtf(real * real + imag * imag);
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

    const float noiseFloor = maxAnaMag * AUDIOX_PITCH_VOCODER_NOISE_FLOOR_RATIO;

    for (uint32_t k = 0U; k < halfBins; ++k) {
        s->synMag[k] = 0.0f;
        s->synFreqAcc[k] = 0.0f;
    }

    for (uint32_t k = 0U; k < halfBins; ++k) {
        if (s->anaMag[k] < noiseFloor) {
            continue;
        }

        const uint32_t mapped = (uint32_t)((float)k * pitchRatio);
        if (mapped >= halfBins) {
            continue;
        }

        s->synMag[mapped] += s->anaMag[k];
        s->synFreqAcc[mapped] += s->anaFreq[k] * pitchRatio * s->anaMag[k];
    }

    for (uint32_t k = 0U; k < halfBins; ++k) {
        float synFreq = (float)k;
        if (s->synMag[k] > 0.0f) {
            synFreq = s->synFreqAcc[k] / s->synMag[k];
        }

        float delta = synFreq - (float)k;
        delta = (2.0f * kPi * delta) / osampF;
        delta += (float)k * expectedPhaseAdvance;

        s->sumPhase[k] += delta;
        const float phase = s->sumPhase[k];

        s->fftWork[k * 2U] = s->synMag[k] * cosf(phase);
        s->fftWork[k * 2U + 1U] = s->synMag[k] * sinf(phase);
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

    const float scale = 2.0f / ((float)(fftSize / 2U) * osampF);
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

    if (inputGain < 0.0f) inputGain = 0.0f;
    if (inputGain > 4.0f) inputGain = 4.0f;
    if (semitoneShift < -AUDIOX_PITCH_VOCODER_MAX_SEMITONES) semitoneShift = -AUDIOX_PITCH_VOCODER_MAX_SEMITONES;
    if (semitoneShift > AUDIOX_PITCH_VOCODER_MAX_SEMITONES) semitoneShift = AUDIOX_PITCH_VOCODER_MAX_SEMITONES;
    if (mix < 0.0f) mix = 0.0f;
    if (mix > 1.0f) mix = 1.0f;
    if (outputGain < 0.0f) outputGain = 0.0f;
    if (outputGain > 4.0f) outputGain = 4.0f;

    PitchState *s = getStateFor(effectId, channel);

    if (!s->inited) {
        memset(s, 0, sizeof(*s));
        for (uint32_t i = 0U; i < PitchState::FFT_SIZE; ++i) {
            s->window[i] = 0.5f - 0.5f * cosf((2.0f * kPi * (float)i) / (float)PitchState::FFT_SIZE);
        }
        s->rover = PitchState::FFT_SIZE - PitchState::HOP_SIZE;
        s->inited = true;
    }

    const float pitchRatio = powf(2.0f, semitoneShift / 12.0f);
    const uint32_t inFifoLatency = PitchState::FFT_SIZE - PitchState::HOP_SIZE;
    const float wetLpfAlpha = expf(-2.0f * kPi * AUDIOX_PITCH_VOCODER_WET_LPF_HZ / (float)SAMPLE_RATE);

    for (uint32_t i = 0; i < frames; ++i) {
        s->inFifo[s->rover] = in[i] * inputGain;

        const uint32_t outIndex = s->rover - inFifoLatency;
        const float wetRaw = s->outFifo[outIndex];
        const float wet = (1.0f - wetLpfAlpha) * wetRaw + wetLpfAlpha * s->wetLpfZ;
        s->wetLpfZ = wet;
        const float dry = in[i] * inputGain;

        float sample = (dry * (1.0f - mix) + wet * mix) * outputGain;
        if (sample > 1.0f) sample = 1.0f;
        if (sample < -1.0f) sample = -1.0f;
        out[i] = sample;

        s->rover++;
        if (s->rover >= PitchState::FFT_SIZE) {
            processFrame(s, pitchRatio);
            s->rover = inFifoLatency;
        }
    }
}

} // namespace audiox::effects
