#ifndef HB_LINKED_PCM_H
#define HB_LINKED_PCM_H

#include "../apps/entrain/platform/audio.h"

/* Zero means the caller can use the regular player. A negative result means
   starting failed after taking ownership of the input. */
int hb_linked_pcm_start(uint32_t rate, uint64_t frames, en_audio_pull_fn pull,
                        void *context, uint32_t volume, bool live);
/* Length becomes known when pull returns zero. Uses a fixed descriptor pool. */
int hb_linked_pcm_start_video(uint32_t rate, en_audio_pull_fn pull, void *context,
                              uint32_t volume, unsigned block_ms);
void hb_linked_pcm_tick(void);
bool hb_linked_pcm_busy(void);
bool hb_linked_pcm_stop(void);
void hb_linked_pcm_pause(bool paused);
void hb_linked_pcm_set_refill_ready(bool ready);
bool hb_linked_pcm_buffering(void);
en_audio_state_t hb_linked_pcm_state(void);
double hb_linked_pcm_elapsed(void);
void hb_linked_pcm_queue_status(unsigned *queued, unsigned *playing_mask);

#endif
