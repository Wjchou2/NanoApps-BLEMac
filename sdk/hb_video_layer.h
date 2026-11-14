#ifndef HB_VIDEO_LAYER_H
#define HB_VIDEO_LAYER_H

#include <stdbool.h>
#include <stdint.h>

#define HB_VIDEO_LAYER_WIDTH 432u
#define HB_VIDEO_LAYER_HEIGHT 240u

/* Run on the UI task. Planes are uncached bus addresses for full-size NV12.
 * Keep the displayed planes until a replacement presents or close succeeds. */
bool hb_video_layer_available(void);
bool hb_video_layer_idle(void);
bool hb_video_layer_present(uint32_t y, uint32_t uv);
/* Show the standard UI for controls while retaining display ownership. */
bool hb_video_layer_show_ui(void);
bool hb_video_layer_close(void);

#endif
