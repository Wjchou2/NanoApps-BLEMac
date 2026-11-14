#include <string.h>
#include "hb_sdk.h"
#include "hb_h264.h"
#include "hb_video_layer.h"
#include "hb_heap.h"
#include "hb_shared.h"
#include "lvgl/lvgl.h"

#define EXPORT_DIR "/Data/VideoProbe"
#define EXPORT_PATH EXPORT_DIR "/39A10023-video-tables.txt"

/* The existing code dump ends here. Known stock method tables are in
 * the following firmware data region, below the app heap. */
#define TABLE_START 0x0867f8e4u
#define TABLE_END 0x08800000u
#define CONTEXT_WORDS 16u

static const struct {
    uint32_t address;
    uint32_t instruction;
    const char *label;
} decoder_entries[] = {
    {0x08232cfcu, 0x4ff0e92du, "configure"},
    {0x08232f28u, 0x4604b510u, "submit wrapper"},
    {0x082328f8u, 0x4ff7e92du, "submit worker"},
    {0x08232f44u, 0x4604b570u, "cleanup"},
};

static lv_obj_t *status_label;

#define LUMA_BYTES (HB_VIDEO_LAYER_WIDTH * HB_VIDEO_LAYER_HEIGHT)
#define FRAME_BYTES (LUMA_BYTES * 3u / 2u)
#define PROBE_CLIP EXPORT_DIR "/test-clip.bin"
#define PROBE_GROUPS 45u

static bool layer_running;
static void *frame_allocation;
static uint8_t *frame;
static lv_timer_t *restore_timer;
static lv_timer_t *motion_timer;
static void *motion_pool;
static uint32_t motion_planes[VIDEO_LAYER_TEST_FRAMES][2];
static unsigned motion_frame, motion_flips, motion_busy;
static bool motion_done;
static const unsigned motion_order[] = {0, 1, 2, 3, 2, 1};
static char clip_button;
static struct {
    bool active, reader_open;
    uint8_t config[VIDEO_H264_MAX_AVCC];
    uint32_t config_size, groups;
    unsigned current, next_frame;
    uint32_t decode_max_us, worst_gap_us, last_present_us;
    uint8_t *group;
    void *next_pool;
    uint32_t next_planes[VIDEO_LAYER_TEST_FRAMES][2];
    const char *error;
} clip_test;
static int (*previous_unload)(void);
static bool ready;

static uint32_t read_word(uint32_t address)
{
    return *(const volatile uint32_t *)(uintptr_t)address;
}

static void restore_display(void)
{
    if (hb_video_layer_close())
        layer_running = false;
}

static bool release_motion(void)
{
    if (layer_running)
        return false;
    hb_h264_test_motion_release(motion_pool);
    motion_pool = NULL;
    hb_h264_test_motion_release(clip_test.next_pool);
    clip_test.next_pool = NULL;
    if (clip_test.reader_open) {
        hb_fs_reader_close();
        clip_test.reader_open = false;
    }
    hb_os_free(clip_test.group);
    clip_test.group = NULL;
    return true;
}

static int prepare_unload(void)
{
    if (!hb_video_layer_idle())
        return 0;
    restore_display();
    if (motion_timer) {
        lv_timer_delete(motion_timer);
        motion_timer = NULL;
    }
    if (!release_motion())
        return 0;
    if (restore_timer) {
        lv_timer_delete(restore_timer);
        restore_timer = NULL;
    }
    hb_os_free(frame_allocation);
    frame_allocation = NULL;
    frame = NULL;
    return previous_unload ? previous_unload() : 1;
}

static void finish_layer_test(lv_timer_t *timer)
{
    if (!hb_video_layer_idle()) {
        lv_timer_set_repeat_count(timer, 1);
        lv_timer_set_period(timer, 33);
        return;
    }
    restore_timer = NULL;
    restore_display();
    lv_label_set_text(status_label, "Still test finished. Look for a 4 x 4 color grid.");
}

static bool read_clip_word(uint32_t *word)
{
    return hb_fs_reader_read(word, sizeof(*word)) == sizeof(*word);
}

static const char *load_clip_group(void **pool, uint32_t planes[VIDEO_LAYER_TEST_FRAMES][2])
{
    uint32_t start = hb_time_uptime_us(), length;
    if (!read_clip_word(&length) || !length || length > VIDEO_LAYER_TEST_FRAMES * (4u + VIDEO_H264_MAX_SAMPLE) ||
        hb_fs_reader_read(clip_test.group, length) != length)
        return "Could not read a test video group.";
    const char *error = hb_h264_test_decode_group(clip_test.config, clip_test.config_size,
                                                  clip_test.group, length, pool, planes);
    uint32_t elapsed = hb_time_uptime_us() - start;
    if (elapsed > clip_test.decode_max_us)
        clip_test.decode_max_us = elapsed;
    return error;
}

static const char *prepare_clip(void)
{
    clip_test.active = true;
    if (!hb_fs_reader_open(PROBE_CLIP))
        return "Could not open the test clip.";
    clip_test.reader_open = true;
    uint32_t magic;
    if (!read_clip_word(&magic) || magic != 0x3150434eu ||
        !read_clip_word(&clip_test.config_size) || !clip_test.config_size ||
        clip_test.config_size > sizeof(clip_test.config) ||
        hb_fs_reader_read(clip_test.config, clip_test.config_size) != clip_test.config_size ||
        !read_clip_word(&clip_test.groups) || clip_test.groups != PROBE_GROUPS)
        return "Invalid test clip header.";
    unsigned bytes = VIDEO_LAYER_TEST_FRAMES * (4u + VIDEO_H264_MAX_SAMPLE);
    if (hb_os_heap_largest() >= bytes + 128u * 1024u)
        clip_test.group = hb_os_alloc(bytes);
    if (!clip_test.group)
        return "Not enough RAM for the test clip.";

    /* The built-in fixture verifies the decoder's NV12 layout. */
    const char *error = hb_h264_test_motion(&motion_pool, motion_planes);
    if (error)
        return error;
    hb_h264_test_motion_release(motion_pool);
    motion_pool = NULL;
    error = load_clip_group(&motion_pool, motion_planes);
    return error ? error : load_clip_group(&clip_test.next_pool, clip_test.next_planes);
}

static void step_clip(void)
{
    bool next_group = clip_test.next_frame == VIDEO_LAYER_TEST_FRAMES;
    if (next_group) {
        if (clip_test.current + 1u == clip_test.groups) {
            restore_display();
            return;
        }
        if (!clip_test.next_pool) {
            clip_test.error = "Missing decoded video group.";
            restore_display();
            return;
        }
    }
    const uint32_t *planes = next_group ? clip_test.next_planes[0] : motion_planes[clip_test.next_frame];
    if (!hb_video_layer_present(planes[0], planes[1])) {
        motion_busy++;
        return;
    }
    if (next_group) {
        hb_h264_test_motion_release(motion_pool);
        motion_pool = clip_test.next_pool;
        clip_test.next_pool = NULL;
        for (unsigned i = 0; i < VIDEO_LAYER_TEST_FRAMES; i++) {
            motion_planes[i][0] = clip_test.next_planes[i][0];
            motion_planes[i][1] = clip_test.next_planes[i][1];
        }
        clip_test.next_frame = 0;
        clip_test.current++;
    }
    uint32_t now = hb_time_uptime_us();
    uint32_t gap = now - clip_test.last_present_us;
    clip_test.last_present_us = now;
    if (gap > clip_test.worst_gap_us)
        clip_test.worst_gap_us = gap;
    motion_flips++;
    clip_test.next_frame++;
    if (clip_test.current + 1u < clip_test.groups && !clip_test.next_pool) {
        clip_test.error = load_clip_group(&clip_test.next_pool, clip_test.next_planes);
        if (clip_test.error)
            motion_done = true;
    }
}

static void step_motion(lv_timer_t *timer)
{
    if (layer_running && !hb_video_layer_idle()) {
        motion_busy++;
        return;
    }
    if (layer_running && motion_done)
        restore_display();
    if (layer_running) {
        if (clip_test.active) {
            step_clip();
            return;
        }
        unsigned next = (motion_frame + 1u) % (sizeof(motion_order) / sizeof(motion_order[0]));
        const uint32_t *planes = motion_planes[motion_order[next]];
        if (!hb_video_layer_present(planes[0], planes[1])) {
            motion_busy++;
        } else {
            motion_frame = next;
            motion_flips++;
        }
        return;
    }
    if (!release_motion())
        return;
    if (restore_timer) {
        lv_timer_delete(restore_timer);
        restore_timer = NULL;
    }
    lv_timer_delete(timer);
    motion_timer = NULL;
    const char *result = clip_test.error;
    if (!result)
        result = !clip_test.active ? "Motion finished." :
            motion_flips == clip_test.groups * VIDEO_LAYER_TEST_FRAMES ? "Clip finished." : "Clip timed out.";
    lv_label_set_text_fmt(status_label, "%s %u refreshes, %u busy.", result, motion_flips, motion_busy);
    if (hb_fs_is_dir(EXPORT_DIR) || hb_fs_mkdir(EXPORT_DIR)) {
        char report[240];
        int size = lv_snprintf(report, sizeof(report),
            "%s\n432 x 240 NV12. Frame changes: %u. Busy: %u. Peak read and decode: %lu us. Longest refresh gap: %lu us.\n",
            result, motion_flips, motion_busy, (unsigned long)clip_test.decode_max_us,
            (unsigned long)clip_test.worst_gap_us);
        if (size > 0 && (unsigned)size < sizeof(report))
            hb_fs_write(clip_test.active ? EXPORT_DIR "/video-layer-clip.txt" : EXPORT_DIR "/video-layer-motion.txt",
                        report, (uint32_t)size);
    }
    clip_test.active = false;
}

static void finish_motion(lv_timer_t *timer)
{
    (void)timer;
    restore_timer = NULL;
    motion_done = true;
}

static void copy_nv12(const uint8_t *y, const uint8_t *u, const uint8_t *v,
                      unsigned y_stride, unsigned uv_stride, unsigned chroma_step, unsigned shared_plane)
{
    volatile uint8_t *dst = frame;
    for (unsigned row = 0; row < HB_VIDEO_LAYER_HEIGHT; row++)
        for (unsigned x = 0; x < HB_VIDEO_LAYER_WIDTH; x++)
            *dst++ = y[row * y_stride + x];
    for (unsigned row = 0; row < HB_VIDEO_LAYER_HEIGHT / 2u; row++)
        for (unsigned x = 0; x < HB_VIDEO_LAYER_WIDTH / 2u; x++) {
            *dst++ = u[row * uv_stride + x * chroma_step];
            *dst++ = v[row * uv_stride + x * chroma_step];
        }
    __asm__ volatile("dsb sy" ::: "memory");
    if (hb_fs_is_dir(EXPORT_DIR) || hb_fs_mkdir(EXPORT_DIR)) {
        char report[140];
        int size = lv_snprintf(report, sizeof(report),
            "Decoder frame copied to NV12. Y stride %u, chroma stride %u, chroma step %u, shared chroma plane %u.\n",
            y_stride, uv_stride, chroma_step, shared_plane);
        if (size > 0 && (unsigned)size < sizeof(report))
            hb_fs_write(EXPORT_DIR "/video-layer-test.txt", report, (uint32_t)size);
    }
    ready = true;
}

static void test_video_layer(lv_event_t *event)
{
    void *selection = lv_event_get_user_data(event);
    bool motion = selection != NULL;
    bool clip = selection == &clip_button;
    if (layer_running || restore_timer || motion_timer || !hb_video_layer_available()) {
        lv_label_set_text(status_label, "Video layer unavailable. Exit Videos and try again.");
        return;
    }
    if (!motion && !frame_allocation && hb_os_heap_largest() >= FRAME_BYTES + 1023u + 128u * 1024u) {
        frame_allocation = hb_os_alloc(FRAME_BYTES + 1023u);
        if (frame_allocation)
            frame = (uint8_t *)((((uintptr_t)frame_allocation + 1023u) & ~1023u) | 0x80000000u);
    }
    if (!motion && !frame) {
        lv_label_set_text(status_label, "Not enough memory for the test frame.");
        return;
    }
    if (motion) {
        memset(&clip_test, 0, sizeof(clip_test));
        const char *error = clip ? prepare_clip() : hb_h264_test_motion(&motion_pool, motion_planes);
        if (error) {
            release_motion();
            lv_label_set_text(status_label, error);
            return;
        }
        motion_timer = lv_timer_create(step_motion, 33, NULL);
        if (!motion_timer) {
            release_motion();
            return;
        }
        motion_frame = motion_flips = motion_busy = 0;
        motion_done = false;
    } else if (!ready) {
        const char *error = hb_h264_test_native_frame(copy_nv12);
        if (error) {
            ready = false;
            lv_label_set_text(status_label, error);
            return;
        }
    }
    restore_timer = lv_timer_create(motion ? finish_motion : finish_layer_test, clip ? 10000 : 3000, NULL);
    if (!restore_timer)
        goto cancel;
    lv_timer_set_repeat_count(restore_timer, 1);

    uint32_t y = motion ? motion_planes[0][0] : (uint32_t)(uintptr_t)frame & 0x7fffffffu;
    uint32_t uv = motion ? motion_planes[0][1] : y + LUMA_BYTES;
    layer_running = true;
    if (hb_video_layer_present(y, uv)) {
        if (motion) {
            motion_flips = 1;
            clip_test.last_present_us = hb_time_uptime_us();
            clip_test.next_frame = 1;
        }
        return;
    }
    restore_display();
    lv_label_set_text(status_label, "Panel skipped the refresh. Tap again.");
    if (layer_running) {
        motion_done = true;
        return;
    }
cancel:
    if (restore_timer) {
        lv_timer_delete(restore_timer);
        restore_timer = NULL;
    }
    if (release_motion() && motion_timer) {
        lv_timer_delete(motion_timer);
        motion_timer = NULL;
    }
}

static bool firmware_matches(void)
{
    for (unsigned i = 0; i < sizeof(decoder_entries) / sizeof(decoder_entries[0]); i++) {
        if (read_word(decoder_entries[i].address) != decoder_entries[i].instruction)
            return false;
    }
    return read_word(0x08232f24u) == 0x38f00000u;
}

static bool export_entries(unsigned *matches)
{
    char line[96];
    const char header[] = "39A10023 video decoder method references\n";
    bool ok = hb_fs_stream_write(header, sizeof(header) - 1);
    for (uint32_t address = TABLE_START; ok && address < TABLE_END; address += 4) {
        uint32_t word = read_word(address);
        for (unsigned i = 0; i < sizeof(decoder_entries) / sizeof(decoder_entries[0]); i++) {
            if (word != (decoder_entries[i].address | 1u))
                continue;
            (*matches)++;
            int length = lv_snprintf(line, sizeof(line), "\n%08lX  %s\n",
                                     (unsigned long)address, decoder_entries[i].label);
            ok = hb_fs_stream_write(line, (uint32_t)length);
            uint32_t start = address - TABLE_START >= CONTEXT_WORDS * 4u
                ? address - CONTEXT_WORDS * 4u : TABLE_START;
            uint32_t end = address + (CONTEXT_WORDS + 1u) * 4u;
            if (end > TABLE_END)
                end = TABLE_END;
            for (uint32_t slot = start; ok && slot < end; slot += 4) {
                length = lv_snprintf(line, sizeof(line), "%08lX  %08lX\n",
                                     (unsigned long)slot, (unsigned long)read_word(slot));
                ok = hb_fs_stream_write(line, (uint32_t)length);
            }
        }
    }
    return ok;
}

static void run_probe(lv_event_t *event)
{
    (void)event;
    if (!firmware_matches()) {
        lv_label_set_text(status_label, "This probe needs firmware 1.1.2 (39A10023).");
        return;
    }
    if ((!hb_fs_is_dir("/Data") && !hb_fs_mkdir("/Data")) ||
        (!hb_fs_is_dir(EXPORT_DIR) && !hb_fs_mkdir(EXPORT_DIR)) ||
        !hb_fs_stream_open(EXPORT_PATH)) {
        lv_label_set_text(status_label, "Could not open the export file.");
        return;
    }
    unsigned matches = 0;
    bool ok = export_entries(&matches);
    bool closed = hb_fs_stream_close();
    if (!ok || !closed) {
        hb_fs_remove(EXPORT_PATH);
        lv_label_set_text(status_label, "Could not save the export.");
        return;
    }
    lv_label_set_text_fmt(status_label, "Saved %u references in Data/VideoProbe.", matches);
}

HB_APP_ENTRY(payload_entry)
{
    previous_unload = hb_shared->prepare_unload;
    hb_shared->prepare_unload = prepare_unload;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Video Probe");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 48);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 208);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "Show a still frame or a moving square on the hardware video layer.");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 108);

    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 192, 44);
    lv_obj_align(button, LV_ALIGN_TOP_MID, 0, 198);
    lv_obj_add_event_cb(button, run_probe, LV_EVENT_CLICKED, NULL);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, "Export tables");
    lv_obj_center(label);

    lv_obj_t *test = lv_button_create(screen);
    lv_obj_set_size(test, 192, 44);
    lv_obj_align(test, LV_ALIGN_TOP_MID, 0, 256);
    lv_obj_add_event_cb(test, test_video_layer, LV_EVENT_CLICKED, NULL);
    label = lv_label_create(test);
    lv_label_set_text(label, "Test video layer");
    lv_obj_center(label);
    lv_obj_t *motion = lv_button_create(screen);
    lv_obj_set_size(motion, 192, 44);
    lv_obj_align(motion, LV_ALIGN_TOP_MID, 0, 312);
    lv_obj_add_event_cb(motion, test_video_layer, LV_EVENT_CLICKED, motion);
    label = lv_label_create(motion);
    lv_label_set_text(label, "Test motion");
    lv_obj_center(label);
    lv_obj_t *clip = lv_button_create(screen);
    lv_obj_set_size(clip, 192, 44);
    lv_obj_align(clip, LV_ALIGN_TOP_MID, 0, 368);
    lv_obj_add_event_cb(clip, test_video_layer, LV_EVENT_CLICKED, &clip_button);
    label = lv_label_create(clip);
    lv_label_set_text(label, "Play test clip");
    lv_obj_center(label);
}
