#include "hb_video_surface.h"
#include "hb_heap.h"
#include <string.h>

#define QUEUE_FRAMES 64u
#define DISPLAY_WIDTH 240u
#define DISPLAY_HEIGHT 432u

static uint8_t *pictures;
static unsigned picture_size;
static uint16_t display[DISPLAY_WIDTH * DISPLAY_HEIGHT] __attribute__((aligned(32)));
static uint16_t *legacy_pixels;
static uint32_t queued, shown = UINT32_MAX;
static lv_obj_t *landscape, *legacy_canvas;
static bool dismissed;
static uint32_t mapped_width, mapped_height;
static uint32_t source_rows[DISPLAY_WIDTH];
static uint16_t source_columns[DISPLAY_HEIGHT];

static void landscape_deleted(lv_event_t *event)
{
    if (lv_event_get_target(event) == legacy_canvas)
        legacy_canvas = NULL;
    landscape = NULL;
}

lv_obj_t *video_landscape_open(lv_event_cb_t close)
{
    if (landscape)
        return NULL;
    landscape = lv_canvas_create(lv_layer_top());
    if (landscape) {
        lv_canvas_set_buffer(landscape, display, DISPLAY_WIDTH, DISPLAY_HEIGHT, LV_COLOR_FORMAT_RGB565);
        lv_obj_align(landscape, LV_ALIGN_CENTER, 0, 0);
        lv_obj_add_flag(landscape, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(landscape, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(landscape, close, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(landscape, landscape_deleted, LV_EVENT_DELETE, NULL);
    }
    return landscape;
}

void video_landscape_present(lv_obj_t *canvas, const uint16_t *pixels, unsigned side)
{
    unsigned crop = side * 9u / 16u;
    video_landscape_present_cropped(canvas, pixels + (side - crop) / 2u * side, side, crop);
}

void video_landscape_present_cropped(lv_obj_t *canvas, const uint16_t *pixels,
                                     unsigned width, unsigned height)
{
    /* The display stays in portrait coordinates. Rotate the picture by
     * 90 degrees and scale it to the entire screen. */
    if (width != mapped_width || height != mapped_height) {
        for (unsigned x = 0; x < DISPLAY_WIDTH; x++)
            source_rows[x] = (DISPLAY_WIDTH - 1u - x) * height / DISPLAY_WIDTH * width;
        for (unsigned y = 0; y < DISPLAY_HEIGHT; y++)
            source_columns[y] = y * width / DISPLAY_HEIGHT;
        mapped_width = width;
        mapped_height = height;
    }
    unsigned previous_x = UINT32_MAX;
    for (unsigned y = 0; y < DISPLAY_HEIGHT; y++) {
        unsigned source_x = source_columns[y];
        uint16_t *row = display + y * DISPLAY_WIDTH;
        if (source_x == previous_x) {
            uint32_t *dest = (uint32_t *)row;
            const uint32_t *previous = (const uint32_t *)(row - DISPLAY_WIDTH);
            for (unsigned x = 0; x < DISPLAY_WIDTH / 2u; x++)
                dest[x] = previous[x];
        } else {
            for (unsigned x = 0; x < DISPLAY_WIDTH; x++)
                row[x] = pixels[source_rows[x] + source_x];
        }
        previous_x = source_x;
    }
    lv_obj_invalidate(canvas);
}

static void dismiss_legacy(lv_event_t *event)
{
    dismissed = true;
    lv_obj_delete(lv_event_get_target(event));
}

void video_stream_reset(void)
{
    if (legacy_canvas)
        lv_obj_delete(legacy_canvas);
    hb_os_free(pictures);
    hb_os_free(legacy_pixels);
    pictures = NULL;
    legacy_pixels = NULL;
    queued = 0;
    shown = UINT32_MAX;
    dismissed = false;
}

bool video_stream_start(unsigned side)
{
    picture_size = side * side;
    uint32_t scratch_size = picture_size * sizeof(*legacy_pixels);
    if (hb_os_heap_largest() < QUEUE_FRAMES * picture_size + scratch_size + 128u * 1024u)
        return false;
    pictures = hb_os_alloc(QUEUE_FRAMES * picture_size);
    legacy_pixels = hb_os_alloc(scratch_size);
    if (pictures && legacy_pixels)
        return true;
    video_stream_reset();
    return false;
}

uint8_t *video_stream_frame(uint32_t index)
{
    queued = index + 1;
    return pictures + (index % QUEUE_FRAMES) * picture_size;
}

static uint16_t rgb565(uint8_t pixel)
{
    unsigned red = pixel >> 5;
    unsigned green = (pixel >> 2) & 7u;
    unsigned blue = pixel & 3u;
    return (uint16_t)(((red * 31u / 7u) << 11) |
                      ((green * 9u) << 5) | (blue * 31u / 3u));
}

void video_stream_present(double elapsed_seconds, unsigned fps, unsigned side)
{
    double time = elapsed_seconds - VIDEO_LEAD_MS / 1000.0;
    if (time < 0 || !queued || dismissed)
        return;
    uint32_t index = (uint32_t)(time * fps);
    if (index >= queued)
        index = queued - 1;
    if (queued > QUEUE_FRAMES && index < queued - QUEUE_FRAMES)
        index = queued - QUEUE_FRAMES;
    if (index == shown)
        return;

    if (!legacy_canvas)
        legacy_canvas = video_landscape_open(dismiss_legacy);
    if (!legacy_canvas)
        return;
    const uint8_t *picture = pictures + (index % QUEUE_FRAMES) * picture_size;
    for (unsigned i = 0; i < side * side; i++)
        legacy_pixels[i] = rgb565(picture[i]);
    shown = index;
    video_landscape_present(legacy_canvas, legacy_pixels, side);
}
