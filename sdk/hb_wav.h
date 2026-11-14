#ifndef HB_WAV_H
#define HB_WAV_H

#include <stdbool.h>
#include <stdint.h>

/* One WAV reader at a time. Output is stereo 16-bit PCM at HB_PCM_RATE.
   Open returns NULL on success or an error to show the user. */
const char *hb_wav_open(const char *path, uint32_t max_size, uint32_t *frames);
/* Play files above 22.05 kHz at 44.1 kHz, and other files at HB_PCM_RATE. */
const char *hb_wav_open_native(const char *path, uint32_t max_size,
                               uint32_t *frames, uint32_t *rate);
uint32_t hb_wav_read(int16_t *output, uint32_t frames);
bool hb_wav_failed(void);
void hb_wav_close(void);

#endif
