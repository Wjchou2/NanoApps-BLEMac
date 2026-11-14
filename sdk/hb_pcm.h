#ifndef HB_PCM_H
#define HB_PCM_H

#include <stdbool.h>
#include <stdint.h>

#define HB_PCM_RATE 22050u

/* The shared playback buffer holds interleaved stereo 16-bit PCM.
 * Reserve and fill it on the UI task before starting playback. */
int16_t *hb_pcm_reserve(uint32_t size, uint32_t headroom);
int16_t *hb_pcm_samples(void);
bool hb_pcm_play(uint32_t size);
bool hb_pcm_playing(void);

/* Also usable as a prepare_unload callback. Returns 0 while the audio
 * task still holds the buffer and descriptor. */
int hb_pcm_release(void);

/* Returns NULL when playback starts, or a message to show the user. */
const char *hb_pcm_play_wav_file(const char *path, uint32_t max_size);

#endif
