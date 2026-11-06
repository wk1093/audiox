#include "audio/alsa_pcm.h"
#include <cassert>

int main() {
    snd_pcm_t *pcm = nullptr;
    snd_pcm_access_t access = SND_PCM_ACCESS_MMAP_INTERLEAVED;
    snd_pcm_uframes_t period = 0, buffer = 0;
    size_t frameBytes = 0;
    unsigned actualRate = 0;
    assert(audio_pcm_open_configured(&pcm, "null", SND_PCM_STREAM_PLAYBACK, 44100,
        2, SND_PCM_FORMAT_S16_LE, &access, 128, 4, &period, &buffer, &frameBytes,
        1, 1, &actualRate) == 0);
    snd_pcm_hw_params_t *params = nullptr;
    assert(snd_pcm_hw_params_malloc(&params) == 0);
    assert(snd_pcm_hw_params_current(pcm, params) == 0);
    unsigned independentlyReadRate = 0;
    assert(snd_pcm_hw_params_get_rate(params, &independentlyReadRate, nullptr) == 0);
    assert(actualRate == independentlyReadRate);
    assert(actualRate == 44100);
    assert(frameBytes == 4);
    snd_pcm_hw_params_free(params);
    assert(snd_pcm_close(pcm) == 0);
}
