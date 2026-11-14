#ifndef VIDEO_STREAM_H
#define VIDEO_STREAM_H

#include "lvgl/lvgl.h"

#define VIDEO_SIDE 72u
#define VIDEO_PIXELS (VIDEO_SIDE * VIDEO_SIDE)
#define VIDEO_FPS 10u
#define VIDEO_WIRE_RATE 7350u
#define VIDEO_AUDIO_FRAMES (VIDEO_WIRE_RATE / VIDEO_FPS)
#define VIDEO_RECORD_BYTES (VIDEO_PIXELS + VIDEO_AUDIO_FRAMES * 2u)
#define VIDEO_HEADER_BYTES 8u
#define VIDEO_LIVE_HEADER_BYTES 24u
#define VIDEO_LIVE_MAX_SIDE 124u
#define VIDEO_LEAD_MS 600u

void video_stream_reset(void);
bool video_stream_start(unsigned side);
uint8_t *video_stream_frame(uint32_t index);
void video_stream_present(double elapsed_seconds, unsigned fps, unsigned side);
lv_obj_t *video_landscape_open(lv_event_cb_t close);
void video_landscape_present(lv_obj_t *canvas, const uint16_t *pixels, unsigned side);
void video_landscape_present_cropped(lv_obj_t *canvas, const uint16_t *pixels,
                                     unsigned width, unsigned height);

#endif
