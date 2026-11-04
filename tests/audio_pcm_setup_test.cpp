#include "audio/alsa_pcm.h"

#include <cassert>
#include <cstdio>
#include <initializer_list>

int main() {
    for (auto stream : {SND_PCM_STREAM_PLAYBACK, SND_PCM_STREAM_CAPTURE}) {
        for (auto access : {SND_PCM_ACCESS_RW_INTERLEAVED, SND_PCM_ACCESS_MMAP_INTERLEAVED}) {
            for (unsigned period : {128U, 256U, 512U, 1024U}) {
                snd_pcm_t *pcm = nullptr;
                assert(snd_pcm_open(&pcm, "null", stream, SND_PCM_NONBLOCK) == 0);
                snd_pcm_uframes_t actualPeriod = 0, actualBuffer = 0;
                assert(audio_pcm_configure_hw_handle(pcm, "null", SAMPLE_RATE, 2,
                    SND_PCM_FORMAT_S16_LE, access, period, 4,
                    &actualPeriod, &actualBuffer, nullptr, 1, 1) == 0);
                snd_pcm_sw_params_t *sw = nullptr;
                assert(snd_pcm_sw_params_malloc(&sw) == 0);
                assert(snd_pcm_sw_params_current(pcm, sw) == 0);
                snd_pcm_uframes_t startThreshold = 0, availMin = 0;
                assert(snd_pcm_sw_params_get_start_threshold(sw, &startThreshold) == 0);
                assert(snd_pcm_sw_params_get_avail_min(sw, &availMin) == 0);
                const auto expected = stream == SND_PCM_STREAM_PLAYBACK
                    ? audio_pcm_playback_start_frames(actualPeriod, actualBuffer) : 1U;
                assert(startThreshold == expected);
                // ALSA may clamp avail_min up to its hardware period.
                assert(availMin >= std::min<snd_pcm_uframes_t>(actualPeriod, BUFFER_FRAMES));
                snd_pcm_sw_params_free(sw);
                assert(snd_pcm_close(pcm) == 0);
            }
        }
    }
    puts("ALSA RW/MMAP playback priming and capture setup verified at every tier");
}
