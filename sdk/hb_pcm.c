#include "hb_pcm.h"
#include "hb_heap.h"
#include "hb_sdk.h"
#include "hb_wav.h"
#include <stddef.h>

#define SFX_PLAYING 0x64u

static uint32_t descriptor[0x80 / sizeof(uint32_t)];
static int16_t *pcm;
static uint32_t pcm_capacity;

bool hb_pcm_playing(void)
{
    return pcm && ((const volatile uint8_t *)descriptor)[SFX_PLAYING];
}

int hb_pcm_release(void)
{
    if (hb_pcm_playing())
        return 0;
    if (pcm) {
        hb_os_free(pcm);
        pcm = NULL;
        pcm_capacity = 0;
    }
    return 1;
}

int16_t *hb_pcm_reserve(uint32_t size, uint32_t headroom)
{
    if (hb_pcm_playing())
        return NULL;
    if (pcm_capacity < size) {
        hb_pcm_release();
        if (headroom && hb_os_heap_largest() < size + headroom)
            return NULL;
        pcm = hb_os_alloc(size);
        if (pcm)
            pcm_capacity = size;
    }
    return pcm;
}

int16_t *hb_pcm_samples(void)
{
    return pcm;
}

static bool play_at_rate(uint32_t size, uint32_t rate)
{
    if (!pcm || !size || size > pcm_capacity || hb_pcm_playing())
        return false;
    /* Same 1.1.2 linear PCM descriptor layout as Entrain. The native
     * voice sets its ownership flag synchronously in setSource. */
    hb_audio_ctor(descriptor);
    descriptor[0x04 / 4] = descriptor[0x08 / 4] = (uint32_t)(uintptr_t)pcm;
    descriptor[0x0c / 4] = size;
    ((uint8_t *)descriptor)[0x10] = 0;  /* Linear PCM. */
    descriptor[0x14 / 4] = rate;
    descriptor[0x18 / 4] = 2;
    descriptor[0x1c / 4] = 16;
    hb_audio_setfields(descriptor, 0x4000);
    hb_audio_play_now(descriptor);
    if (hb_pcm_playing())
        return true;
    hb_pcm_release();
    return false;
}

bool hb_pcm_play(uint32_t size)
{
    return play_at_rate(size, HB_PCM_RATE);
}

const char *hb_pcm_play_wav_file(const char *path, uint32_t max_size)
{
    if (!hb_pcm_release())
        return "Audio is already playing.";
    uint32_t frames, rate;
    const char *error = hb_wav_open_native(path, max_size, &frames, &rate);
    if (error)
        return error;

    if (frames > 30u * rate) {
        hb_wav_close();
        return "Open this file in Audio Player.";
    }
    uint32_t pcm_size = frames * 4;
    int16_t *samples = hb_pcm_reserve(pcm_size, 128u * 1024u);
    if (!samples) {
        hb_wav_close();
        return "Not enough memory to play this file.";
    }
    uint32_t read = hb_wav_read(samples, frames);
    hb_wav_close();
    if (read != frames) {
        hb_pcm_release();
        return "Could not read the WAV file.";
    }
    return play_at_rate(pcm_size, rate) ? NULL : "Could not start audio playback.";
}
