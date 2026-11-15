#ifndef HB_H264_H
#define HB_H264_H

#include <stdint.h>
#include <stdbool.h>

#define VIDEO_H264_MAX_FRAMES 12u
#define VIDEO_H264_NATIVE_GROUP_FRAMES 4u
#define VIDEO_H264_MAX_SAMPLE (16u * 1024u)
#define VIDEO_H264_MAX_AVCC 128u
#define VIDEO_H264_AUDIO_RATE 11025u
#define VIDEO_H264_AUDIO_ADPCM 0x80000000u

static inline bool hb_video_frame_rate_supported(uint32_t fps)
{
    return fps == 5u || fps == 10u || fps == 15u || fps == 22u || fps == 30u;
}

/* Run on the UI task. Initialization installs an unload hook if the app
 * has none. Apps with their own hook must call hb_h264_prepare_unload. */
bool hb_h264_init(void);
int hb_h264_prepare_unload(void);
#ifdef HB_VIDEO_LAYER_TEST
/* The sample planes are only valid for the duration of the callback. */
const char *hb_h264_test_native_frame(void (*copy)(const uint8_t *, const uint8_t *, const uint8_t *,
                                                   unsigned, unsigned, unsigned, unsigned));
#define VIDEO_LAYER_TEST_FRAMES 4u
/* Keep the pool alive until scanout has released its last frame. */
const char *hb_h264_test_motion(void **pool, uint32_t planes[VIDEO_LAYER_TEST_FRAMES][2]);
const char *hb_h264_test_decode_group(const uint8_t *config, uint32_t config_size,
                                      const uint8_t *group, uint32_t length, void **pool,
                                      uint32_t planes[VIDEO_LAYER_TEST_FRAMES][2]);
void hb_h264_test_motion_release(void *pool);
#endif
/* Open an NVH1–NVH5 clip produced by Nano Bluetooth Link. The original file
 * stays on disk. Playback and the full-screen view are managed by the SDK. */
const char *hb_h264_play_file(const char *path);
/* Returns false while the display still owns a frame. Retry before unload. */
bool hb_h264_stop(void);
#define VIDEO_H264_RECEIVE_PATH "/Data/.VideoStream-clip.tmp"
/* Takes ownership of the complete, checksummed temporary file. */
bool hb_h264_can_receive(void);
const char *hb_h264_play_received(uint32_t length);

/* Call init before standalone live playback. Config is AVCC SPS/PPS.
 * Count zero permits an unknown length until live_set_final_count.
 * A group contains up to 12 length-prefixed AVCC samples, then mu-law audio.
 * Set VIDEO_H264_AUDIO_ADPCM in channels for independent IMA ADPCM blocks.
 * Start each group with a keyframe. Poll tick on the UI task. */
bool hb_h264_live_begin(const uint8_t *config, uint32_t size, uint32_t count,
                           uint32_t fps, uint32_t rate, uint32_t channels, unsigned group_frames);
void hb_h264_live_stop(void);
bool hb_h264_live_room(void);
/* Submit one complete group from memory on the UI task. */
const char *hb_h264_live_submit(const void *data, uint32_t length);
const char *hb_h264_live_group(bool (*read)(void *, uint32_t, void *), void *context, uint32_t length);
/* Queue one group for hardware decode, then call advance once per UI update
 * to prepare a frame for display. Only completed groups enter the playback queue. */
const char *hb_h264_live_queue(bool (*read)(void *, uint32_t, void *), void *context, uint32_t length);
bool hb_h264_live_group_pending(void);
const char *hb_h264_live_advance(void);
/* Returns -1 for failure, 0 while active, or 1 on completion. */
int hb_h264_live_tick(void);
const char *hb_h264_live_error(void);
bool hb_h264_live_playing(void);
#ifdef NANO_VIDEO_STREAM
bool hb_h264_live_paused(void);
#endif
uint32_t hb_h264_live_buffer_ms(void);
bool hb_h264_live_cancelled(void);
bool hb_h264_live_complete(void);

bool hb_h264_live_set_final_count(uint32_t count);
bool hb_h264_live_finish(void);
const char *hb_h264_play_groups(uint32_t count, uint32_t fps, uint32_t rate,
                                  uint32_t channels, const uint8_t *config, uint32_t size,
                                  unsigned group_frames);
#endif
