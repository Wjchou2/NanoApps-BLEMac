#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_shared.h"
#include "hb_mulaw.h"
#include "hb_ima_adpcm.h"
#include "lvgl/lvgl.h"
#include "hb_h264.h"
#include "hb_h264_fixture.h"
#include "hb_h264_native_fixture.h"
#ifdef HB_VIDEO_LAYER_TEST
#include "hb_h264_motion_fixture.h"
#endif
#include "hb_video_surface.h"
#ifdef HB_VIDEO_LAYER
#include "hb_video_layer.h"
#endif
#include "../apps/entrain/platform/audio.h"

#include <stddef.h>
#include <string.h>

/* These are the pool lifecycle and aligned allocation calls used by the
 * stock video player on nano 7G firmware 39A10023. */
#define VIDEO_POOL_CREATE  0x0840df20u
#define VIDEO_POOL_ALLOC   0x0840c26eu
#define VIDEO_POOL_RELEASE 0x084110fcu
#define STOCK_VIDEO_OWNER  0x08811d74u
#define STOCK_VIDEO_POOL   0x08811d78u
#define CODEC_ADDRESS      0x08998974u
#define CODEC_VTABLE       0x087a7854u
#define CODEC_GET          0x080bd5f0u
#define CODEC_CONFIGURE    0x08232cfcu
#define CODEC_CLEANUP      0x08232f44u
#define CODEC_SUBMIT       0x08232f28u

/* Keep each frame's DMA planes allocated until decoder cleanup. P frames
 * can refer to an earlier output even after submission has returned. */
#define POOL_BYTES        (3u * 1024u * 1024u)
#define WORKSPACE_A_BYTES 0x25c5cu
#define WORKSPACE_B_BYTES 0x2940u
#define FRAME_SIDE        128u
#define CACHE_HEIGHT      (FRAME_SIDE * 9u / 16u)
#define CACHE_ROWS_START  ((FRAME_SIDE - CACHE_HEIGHT) / 2u)
#define FRAME_COUNT       (sizeof(test_frames) / sizeof(test_frames[0]))
#define DEFAULT_FPS       5u
#define DMA_ALIGNMENT     1024u
#define PLANE_BYTES       (64u * 1024u)
#define EMPTY_LUMA        0xcdu
#define NATIVE_WIDTH      432u
#define NATIVE_HEIGHT     240u
#define NATIVE_LUMA_BYTES (128u * 1024u)
#define NATIVE_FRAME_BYTES (VIDEO_H264_MAX_SAMPLE + NATIVE_LUMA_BYTES + 2u * PLANE_BYTES)
#define NATIVE_POOL_OVERHEAD (512u * 1024u)
#define NATIVE_POOL_BYTES (NATIVE_POOL_OVERHEAD + VIDEO_H264_NATIVE_GROUP_FRAMES * NATIVE_FRAME_BYTES)
#define DECODER_HEAP_MARGIN (128u * 1024u)
#define PLAYBACK_HEAP_MARGIN (128u * 1024u)

#ifdef HB_VIDEO_LAYER
#define NV12_Y_BYTES (NATIVE_WIDTH * NATIVE_HEIGHT)
#define NV12_Y_SPAN ((NV12_Y_BYTES + DMA_ALIGNMENT - 1u) & ~(DMA_ALIGNMENT - 1u))
#define NV12_FRAME_SPAN ((NV12_Y_SPAN + NV12_Y_BYTES / 2u + DMA_ALIGNMENT - 1u) & ~(DMA_ALIGNMENT - 1u))
_Static_assert(NATIVE_WIDTH == HB_VIDEO_LAYER_WIDTH && NATIVE_HEIGHT == HB_VIDEO_LAYER_HEIGHT,
               "NV12 frame and video layer geometry must agree");
#endif

typedef void *(*pool_create_fn)(uint32_t bytes, const char *name, unsigned flags);
typedef void *(*pool_alloc_fn)(void *pool, uint32_t bytes, unsigned alignment, unsigned uncached);
typedef void (*pool_release_fn)(void *pool);

typedef struct {
    const uint8_t *data;
    uint32_t size;
} h264_sample;

typedef struct {
    unsigned width, height;
    h264_sample config, sample;
} video_mode;

static const video_mode video_modes[] = {
    {192, 112, {native_192_avcc, sizeof(native_192_avcc)}, {native_192_idr, sizeof(native_192_idr)}},
    {256, 144, {native_256_avcc, sizeof(native_256_avcc)}, {native_256_idr, sizeof(native_256_idr)}},
    {320, 176, {native_320_avcc, sizeof(native_320_avcc)}, {native_320_idr, sizeof(native_320_idr)}},
    {NATIVE_WIDTH, NATIVE_HEIGHT, {native_avcc, sizeof(native_avcc)}, {native_idr, sizeof(native_idr)}},
};

typedef struct {
    h264_sample config;
    h264_sample samples[VIDEO_H264_MAX_FRAMES];
    uint16_t *cropped_output;
    unsigned count, fps;
    unsigned width, height;
    bool fixture, calibration, native;
    void (*service_audio)(void);
} h264_clip;

typedef struct {
    uint32_t reserved[13];
    const uint8_t *avcc;        /* CPU-visible bus address */
    uint32_t avcc_size;
    uint32_t reserved2[11];
    uint32_t instance;
} codec_description;

_Static_assert(offsetof(codec_description, avcc) == 0x34 &&
               offsetof(codec_description, instance) == 0x68, "stock codec layout");

typedef struct decoder_frame decoder_frame;
typedef void (*frame_method)(decoder_frame *, unsigned reason);
struct decoder_frame {
    const frame_method *methods;
    uint32_t reserved[11];
    const uint8_t *cr;
    uint32_t reserved2[11];
    const uint8_t *y;
    uint32_t reserved3[2];
    const uint8_t *cb;
    unsigned references;
};

_Static_assert(offsetof(decoder_frame, cr) == 0x30 &&
               offsetof(decoder_frame, y) == 0x60 &&
               offsetof(decoder_frame, cb) == 0x6c, "stock frame plane offsets");

static void retain_frame(decoder_frame *frame, unsigned reason)
{
    (void)reason;
    frame->references++;
}

static void release_frame(decoder_frame *frame, unsigned reason)
{
    (void)reason;
    if (frame->references)
        frame->references--;
}

/* The stock frame manager calls these two slots to manage the output frame. */
static const frame_method frame_methods[] = {
    [0x1c / 4] = retain_frame,
    [0x20 / 4] = release_frame,
};

/* The driver caches the register base, DMA addresses, and the pool and
 * workspaces it borrows from it. Restore these before releasing the pool. */
static const uintptr_t driver_slots[] = {
    0x088117fcu, 0x08811800u, 0x08811804u, 0x08811810u, 0x08811858u, 0x088118e0u,
};

static uint16_t (*preview)[FRAME_SIDE * FRAME_SIDE];
static char log_text[4096];
static unsigned log_used;
static const char *log_path;
static bool disk_checkpoints;
static bool capture_checkpoints;
static lv_obj_t *preview_dialog;

typedef struct {
    unsigned stride, step;
    unsigned cb_plane, cr_plane, cb_offset, cr_offset;
} chroma_layout;

static chroma_layout chroma_format;

#ifdef NANO_YOUTUBE_VIDEO
typedef struct {
    uint32_t calls, peak_us;
    uint64_t total_us;
} video_timing;

static struct {
    video_timing prepare, submit, convert, group, read, draw;
    uint32_t presented, skipped, stalls, refresh_retries;
    bool active, buffering;
} video_perf;

static void measure_duration(video_timing *timing, uint32_t elapsed)
{
    if (!video_perf.active)
        return;
    timing->calls++;
    timing->total_us += elapsed;
    if (elapsed > timing->peak_us)
        timing->peak_us = elapsed;
}

static void measure_time(video_timing *timing, uint32_t start)
{
    measure_duration(timing, hb_time_uptime_us() - start);
}

static void save_video_timings(unsigned fps, bool from_disk, unsigned queue_frames, bool direct_display)
{
    char report[768];
    int length = lv_snprintf(report, sizeof(report),
        "YouTube Streamer playback\nMode: %s, %u FPS\nDisplay: %s\n"
        "Decoded frame queue capacity: %u frames\n"
        "Frames drawn: %lu, frames skipped: %lu, buffer stalls: %lu\n"
        "Panel refresh retries: %lu\n"
        "Stage                    Calls     Mean us     Peak us\n",
        from_disk ? "After Transfer" : "While Sending", fps,
        direct_display ? "Hardware NV12 layer" : "RGB canvas", queue_frames,
        (unsigned long)video_perf.presented, (unsigned long)video_perf.skipped,
        (unsigned long)video_perf.stalls, (unsigned long)video_perf.refresh_retries);
    const struct {
        const char *label;
        const video_timing *timing;
    } rows[] = {
        {"DMA preparation/frame", &video_perf.prepare},
        {"Decoder submit/frame", &video_perf.submit},
        {direct_display ? "NV12 copy/frame" : "Color conversion/frame", &video_perf.convert},
        {"Decode work/group", &video_perf.group},
        {"Disk read/group", &video_perf.read},
        {direct_display ? "Panel refresh/frame" : "Canvas update/frame", &video_perf.draw},
    };
    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        if (length < 0 || (unsigned)length >= sizeof(report))
            return;
        const video_timing *t = rows[i].timing;
        unsigned long mean = t->calls ? t->total_us / t->calls : 0;
        int added = lv_snprintf(report + length, sizeof(report) - length,
                                "%-23s %6lu %11lu %11lu\n", rows[i].label,
                                (unsigned long)t->calls, mean, (unsigned long)t->peak_us);
        if (added < 0)
            return;
        length += added;
    }
    if ((unsigned)length < sizeof(report))
        hb_fs_write(from_disk ? "/Data/YouTubeStreamer-after-transfer.txt"
                              : "/Data/YouTubeStreamer-while-sending.txt", report, length);
}

static void fill_decoder_buffer(void *buffer, uint32_t word, unsigned bytes)
{
    /* The codec buffers are aligned uncached DMA memory. Keep the same
     * contents as the byte clears, but issue a word store for four bytes. */
    volatile uint32_t *out = buffer;
    for (unsigned i = 0; i < bytes / sizeof(*out); i++)
        out[i] = word;
}

_Static_assert(VIDEO_H264_MAX_SAMPLE % 4u == 0 && PLANE_BYTES % 4u == 0,
               "Decoder buffers must be word aligned");
#endif

static bool checkpoint(const char *stage, uint32_t first, uint32_t second)
{
    hb_trace_log(stage, first, second);
    if (!disk_checkpoints && !capture_checkpoints)
        return true;
    unsigned remaining = sizeof(log_text) - log_used;
    int written = lv_snprintf(log_text + log_used, remaining, "%s %08lX %08lX\n",
                              stage, (unsigned long)first, (unsigned long)second);
    if (written <= 0 || (unsigned)written >= remaining)
        return false;
    log_used += (unsigned)written;
    return !disk_checkpoints || hb_fs_write(log_path, log_text, log_used);
}

static uint32_t read_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}

static void write_word(uintptr_t address, uint32_t value)
{
    *(volatile uint32_t *)address = value;
}

static bool firmware_matches(void)
{
    /* The allocation entry is halfword aligned. Check aligned instructions
     * inside the routine to avoid an unaligned memory access in this probe. */
    return read_word(VIDEO_POOL_CREATE) == 0x41f0e92du &&
           read_word(VIDEO_POOL_ALLOC + 2u) == 0x94002401u &&
           read_word(VIDEO_POOL_RELEASE) == 0x4605b570u &&
           read_word(0x08497788u) == WORKSPACE_A_BYTES &&
           read_word(CODEC_GET) == 0x4c0cb510u &&
           read_word(CODEC_CONFIGURE) == 0x4ff0e92du &&
           read_word(CODEC_CLEANUP) == 0x4604b570u &&
           read_word(CODEC_SUBMIT) == 0x4604b510u &&
           read_word(0x082328f8u) == 0x4ff7e92du &&
           read_word(CODEC_VTABLE + 0x10u) == (CODEC_CONFIGURE | 1u) &&
           read_word(CODEC_VTABLE + 0x14u) == (CODEC_CLEANUP | 1u) &&
           read_word(CODEC_VTABLE + 0x40u) == (CODEC_SUBMIT | 1u) &&
           read_word(CODEC_VTABLE + 0x5cu) == 0x082328ddu;
}

static bool in_sdram(uintptr_t address, uint32_t bytes)
{
    return address >= 0x08000000u && address <= 0x0c000000u - bytes;
}

static bool workspace_valid(uintptr_t alias, uint32_t bytes)
{
    return (alias & 0x80000000u) && !(alias & 3u) &&
           in_sdram(alias & 0x7fffffffu, bytes);
}

static bool codec_has_resources(const volatile uint32_t *codec)
{
    return codec[2] || codec[11] || codec[12] || codec[15];
}

static const char *submit_frame(void *pool, volatile uint32_t *codec,
                                decoder_frame *frame, const h264_sample *sample, unsigned index,
                                bool native)
{
#ifdef NANO_YOUTUBE_VIDEO
    uint32_t started = hb_time_uptime_us();
#endif
    const uint32_t input_size = sample->size;
    unsigned luma_bytes = native ? NATIVE_LUMA_BYTES : PLANE_BYTES;
    unsigned frame_bytes = VIDEO_H264_MAX_SAMPLE + luma_bytes + 2u * PLANE_BYTES;
    if (!checkpoint("FrameNew", index + 1u, frame_bytes))
        return "Could not save the frame allocation checkpoint.";
    pool_alloc_fn allocate = (pool_alloc_fn)(VIDEO_POOL_ALLOC | 1u);
    uintptr_t alias = (uintptr_t)allocate(pool, frame_bytes, 3, 1);
    checkpoint("FrameRx", index + 1u, alias);
    if (!workspace_valid(alias, frame_bytes) || (alias & (DMA_ALIGNMENT - 1u)))
        return "Could not allocate the aligned frame buffer.";

    uint8_t *input = (uint8_t *)alias;
    uint8_t *planes = input + VIDEO_H264_MAX_SAMPLE;
#ifdef NANO_YOUTUBE_VIDEO
    fill_decoder_buffer(input, 0, VIDEO_H264_MAX_SAMPLE);
    fill_decoder_buffer(planes, EMPTY_LUMA * 0x01010101u, luma_bytes + 2u * PLANE_BYTES);
#else
    memset(input, 0, VIDEO_H264_MAX_SAMPLE);
    memset(planes, EMPTY_LUMA, luma_bytes + 2u * PLANE_BYTES);
#endif
    memcpy(input, sample->data, input_size);
    frame->methods = frame_methods;
    frame->y = planes;
    frame->cb = planes + luma_bytes;
    frame->cr = planes + luma_bytes + PLANE_BYTES;
    __asm__ volatile("dsb sy" ::: "memory");

    typedef int (*codec_submit_fn)(volatile uint32_t *, const uint8_t *, uint32_t,
                                   decoder_frame *, decoder_frame *);
    if (!checkpoint("Submit", index + 1u, input_size))
        return "Could not save the frame submission checkpoint.";
#ifdef NANO_YOUTUBE_VIDEO
    measure_time(&video_perf.prepare, started);
    started = hb_time_uptime_us();
#endif
    int result = ((codec_submit_fn)(CODEC_SUBMIT | 1u))(codec, input, input_size,
                                                     frame, NULL);
#ifdef NANO_YOUTUBE_VIDEO
    measure_time(&video_perf.submit, started);
#endif
    checkpoint("SubmitRx", (uint32_t)result, frame->references);
    return result == 0 ? NULL : "H.264 frame submission failed. See the diagnostic log.";
}

static bool check_fixture(const volatile uint8_t *luma, unsigned index)
{
    unsigned samples = 0;
    bool matches = true;
    for (unsigned i = 0; i < FRAME_COUNT; i++) {
        unsigned pixel = luma[32u * FRAME_SIDE + 24u + 16u * i];
        unsigned expected = i == index ? 220u : 48u;
        if (pixel + 12u < expected || pixel > expected + 12u)
            matches = false;
        if (pixel >= 160u)
            samples |= 1u << i;
    }
    checkpoint("Luma", index + 1u, samples);
    return matches;
}

static inline __attribute__((always_inline)) unsigned color_channel(int value)
{
#if defined(__arm__)
    unsigned result;
    /* Cortex-A8 can shift and clamp to an unsigned byte in one instruction. */
    __asm__("usat %0, #8, %1, asr #8" : "=r"(result) : "r"(value) : "cc");
    return result;
#else
    value >>= 8;
    return value < 0 ? 0u : value > 255 ? 255u : (unsigned)value;
#endif
}

static inline __attribute__((always_inline)) uint16_t rgb565_from_luma(uint8_t y, int red, int green, int blue)
{
    int luma = 298 * ((int)y - 16);
    unsigned r = color_channel(luma + red);
    unsigned g = color_channel(luma + green);
    unsigned b = color_channel(luma + blue);
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static void measure_chroma(const decoder_frame *frame)
{
    static const uint8_t expected_cb[] = {
        48, 208, 88, 168, 168, 88, 208, 48, 88, 48, 168, 208, 208, 168, 48, 88
    };
    static const uint8_t expected_cr[] = {
        192, 64, 224, 96, 96, 224, 64, 192, 64, 192, 96, 224, 224, 96, 192, 64
    };
    const volatile uint8_t *planes[] = {frame->cb, frame->cr};
    unsigned best = UINT32_MAX;
    chroma_layout selected = {0};
    /* Firmware reserves half a luma plane for each chroma output. Check
     * their pitch and component order against the decoded color fixture. */
    for (unsigned stride = 64; stride <= 128; stride *= 2) {
        for (unsigned step = 1; step <= stride / 64; step++) {
            for (unsigned mode = 0; mode < 6; mode++) {
                if (mode >= 2 && step != 2)
                    continue;
                chroma_layout layout = {.stride = stride, .step = step};
                if (mode < 2) {
                    layout.cb_plane = mode;
                    layout.cr_plane = 1u - mode;
                } else {
                    layout.cb_plane = layout.cr_plane = (mode - 2u) / 2u;
                    layout.cb_offset = mode & 1u;
                    layout.cr_offset = 1u - layout.cb_offset;
                }
                unsigned error = 0;
                for (unsigned i = 0; i < 16; i++) {
                    unsigned row = 8u + (i / 4u) * 16u;
                    unsigned col = 8u + (i % 4u) * 16u;
                    unsigned pos = row * stride + col * step;
                    int cb = (int)planes[layout.cb_plane][pos + layout.cb_offset] - expected_cb[i];
                    int cr = (int)planes[layout.cr_plane][pos + layout.cr_offset] - expected_cr[i];
                    error += (unsigned)(cb < 0 ? -cb : cb) + (unsigned)(cr < 0 ? -cr : cr);
                }
                if (error < best) {
                    best = error;
                    selected = layout;
                }
            }
        }
    }
    if (best <= 32u * 12u)
        chroma_format = selected;
    checkpoint("Chroma", best, chroma_format.stride);
    checkpoint("Planes", (selected.cb_plane << 8) | selected.cb_offset,
               (selected.cr_plane << 8) | selected.cr_offset | (selected.step << 16));
}

static chroma_layout native_chroma_format;
static unsigned native_luma_stride;

static void copy_dma_words(uint32_t *out, const volatile uint8_t *in, unsigned bytes)
{
    const volatile uint32_t *words = (const volatile uint32_t *)in;
    for (unsigned i = 0; i < bytes / sizeof(*out); i++)
        out[i] = words[i];
}

static bool measure_native_layout(const decoder_frame *frame, unsigned width, unsigned height)
{
    static const uint8_t expected_y[] = {
        52, 192, 104, 160, 160, 104, 192, 52, 104, 52, 160, 192, 192, 160, 52, 104
    };
    static const uint8_t expected_cb[] = {
        48, 208, 88, 168, 168, 88, 208, 48, 88, 48, 168, 208, 208, 168, 48, 88
    };
    static const uint8_t expected_cr[] = {
        192, 64, 224, 96, 224, 64, 96, 192, 96, 224, 192, 64, 224, 192, 64, 96
    };
    const unsigned pitches[] = {width, (width + 31u) & ~31u, (width + 63u) & ~63u,
                               (width + 127u) & ~127u, (width + 255u) & ~255u,
                               224u, 288u, 352u, 480u};
    const volatile uint8_t *y = frame->y;
    const volatile uint8_t *planes[] = {frame->cb, frame->cr};
    unsigned best_y = UINT32_MAX, best_c = UINT32_MAX, pitch_y = 0;
    chroma_layout chroma = {0};
    for (unsigned p = 0; p < sizeof(pitches) / sizeof(*pitches); p++) {
        unsigned pitch = pitches[p], error = 0;
        if (pitch < width)
            continue;
        for (unsigned i = 0; i < 16; i++) {
            unsigned row = (2 * (i / 4) + 1) * height / 8;
            unsigned col = (2 * (i % 4) + 1) * width / 8;
            int delta = (int)y[row * pitch + col] - expected_y[i];
            error += delta < 0 ? -delta : delta;
        }
        if (error < best_y) {
            best_y = error;
            pitch_y = pitch;
        }
        for (unsigned mode = 0; mode < 8; mode++) {
            bool interleaved = mode >= 4;
            unsigned stride = interleaved || mode >= 2 ? pitch : pitch / 2u;
            unsigned step = interleaved ? 2 : 1;
            chroma_layout layout = {.stride = stride, .step = step,
                                    .cb_plane = mode & 1u, .cr_plane = 1u - (mode & 1u)};
            if (interleaved) {
                layout.cb_plane = layout.cr_plane = (mode - 4u) / 2u;
                layout.cb_offset = mode & 1u;
                layout.cr_offset = 1u - layout.cb_offset;
            }
            error = 0;
            for (unsigned i = 0; i < 16; i++) {
                unsigned row = (2 * (i / 4) + 1) * height / 16;
                unsigned col = (2 * (i % 4) + 1) * width / 16;
                unsigned pos = row * stride + col * step;
                int cb = (int)planes[layout.cb_plane][pos + layout.cb_offset] - expected_cb[i];
                int cr = (int)planes[layout.cr_plane][pos + layout.cr_offset] - expected_cr[i];
                error += (cb < 0 ? -cb : cb) + (cr < 0 ? -cr : cr);
            }
            if (error < best_c) {
                best_c = error;
                chroma = layout;
            }
        }
    }
    checkpoint("NativeY", pitch_y, best_y);
    checkpoint("NativeC", chroma.stride, best_c);
    if (best_y > 16u * 10u || best_c > 32u * 10u)
        return false;
    native_luma_stride = pitch_y;
    native_chroma_format = chroma;
    return true;
}

static void convert_frame(const decoder_frame *frame, uint16_t *output, unsigned first_row,
                          unsigned width, unsigned height, bool grayscale, bool native)
{
    const volatile uint8_t *y_plane = frame->y;
    const volatile uint8_t *planes[] = {frame->cb, frame->cr};
    const unsigned pitch = native ? native_luma_stride : FRAME_SIDE;
    const chroma_layout layout = native ? native_chroma_format : chroma_format;
    if (!grayscale && layout.stride && !((first_row | height) & 1u)) {
        const volatile uint8_t *cb_plane = planes[layout.cb_plane];
        const volatile uint8_t *cr_plane = planes[layout.cr_plane];
        /* One chroma pair serves a 2x2 block. Compute its contribution once. */
        for (unsigned y = first_row; y < first_row + height; y += 2) {
            const volatile uint8_t *row = y_plane + y * pitch;
            const volatile uint8_t *next = row + pitch;
            unsigned chroma = y / 2u * layout.stride;
            const volatile uint8_t *cb_row = cb_plane + chroma;
            const volatile uint8_t *cr_row = cr_plane + chroma;
            /* The DMA alias is uncached. Read whole words, then unpack from
               cached stack rows so 432 x 240 does not need a full frame copy. */
            uint32_t luma[2][NATIVE_WIDTH / 4], chroma_rows[2][NATIVE_WIDTH / 4];
            if (native) {
                copy_dma_words(luma[0], row, width);
                copy_dma_words(luma[1], next, width);
                unsigned bytes = width / 2u * layout.step;
                copy_dma_words(chroma_rows[0], cb_row, bytes);
                if (cb_row != cr_row)
                    copy_dma_words(chroma_rows[1], cr_row, bytes);
                cr_row = (const uint8_t *)chroma_rows[cb_row != cr_row];
                cb_row = (const uint8_t *)chroma_rows[0];
                row = (const uint8_t *)luma[0];
                next = (const uint8_t *)luma[1];
            }
            cb_row += layout.cb_offset;
            cr_row += layout.cr_offset;
            chroma = 0;
            for (unsigned x = 0; x < width; x += 2) {
                int cb = (int)cb_row[chroma] - 128;
                int cr = (int)cr_row[chroma] - 128;
                int red = 128 + 409 * cr;
                int green = 128 - 100 * cb - 208 * cr;
                int blue = 128 + 516 * cb;
                chroma += layout.step;
                output[x] = rgb565_from_luma(row[x], red, green, blue);
                output[x + 1] = rgb565_from_luma(row[x + 1], red, green, blue);
                output[x + width] = rgb565_from_luma(next[x], red, green, blue);
                output[x + width + 1] = rgb565_from_luma(next[x + 1], red, green, blue);
            }
            output += 2u * width;
        }
        return;
    }
    for (unsigned y = first_row; y < first_row + height; y++) {
        const volatile uint8_t *row = y_plane + y * pitch;
        unsigned chroma = (y / 2u) * layout.stride;
        for (unsigned x = 0; x < width; x += 2) {
            int red = 128, green = 128, blue = 128;
            if (!grayscale && layout.stride) {
                int cb = (int)planes[layout.cb_plane][chroma + layout.cb_offset] - 128;
                int cr = (int)planes[layout.cr_plane][chroma + layout.cr_offset] - 128;
                red += 409 * cr;
                green -= 100 * cb + 208 * cr;
                blue += 516 * cb;
            }
            chroma += layout.step;
            for (unsigned pixel = x; pixel < x + 2; pixel++)
                *output++ = rgb565_from_luma(row[pixel], red, green, blue);
        }
    }
}

static const char *decode_frames(void *pool, const h264_clip *clip, decoder_frame *frames)
{
    pool_alloc_fn allocate = (pool_alloc_fn)(VIDEO_POOL_ALLOC | 1u);
    if (!checkpoint("AvccNew", clip->config.size, 0))
        return "Could not save the input allocation checkpoint.";
    uintptr_t alias = (uintptr_t)allocate(pool, 1024, 3, 1);
    checkpoint("AvccRx", alias, 0);
    if (!workspace_valid(alias, 1024))
        return "Could not allocate the decoder input.";
    memset((void *)alias, 0, 1024);
    memcpy((void *)alias, clip->config.data, clip->config.size);
    __asm__ volatile("dsb sy" ::: "memory");

    typedef volatile uint32_t *(*codec_get_fn)(void);
    typedef int (*codec_configure_fn)(volatile uint32_t *, const codec_description *);
    typedef void (*codec_cleanup_fn)(volatile uint32_t *);
    if (!checkpoint("GetCodec", 0, 0))
        return "Could not save the codec lookup checkpoint.";
    volatile uint32_t *codec = ((codec_get_fn)(CODEC_GET | 1u))();
    if ((uintptr_t)codec != CODEC_ADDRESS || codec[0] != CODEC_VTABLE)
        return "Stock H.264 decoder is unavailable.";

    uintptr_t bus = alias & 0x7fffffffu;
    if (!checkpoint("Config", bus, clip->config.size))
        return "Could not save the decoder setup checkpoint.";
    if (read_word(STOCK_VIDEO_OWNER) || codec_has_resources(codec) || hb_media_state() == 0)
        return "Exit stock playback before testing.";

    uint32_t saved_codec[16];
    uint32_t saved_driver[sizeof(driver_slots) / sizeof(driver_slots[0])];
    for (unsigned i = 1; i < 16; i++)
        saved_codec[i] = codec[i];
    for (unsigned i = 0; i < sizeof(saved_driver) / sizeof(saved_driver[0]); i++)
        saved_driver[i] = read_word(driver_slots[i]);
    const uint32_t saved_pool = read_word(STOCK_VIDEO_POOL);
    const codec_description description __attribute__((aligned(8))) = {
        .avcc = (const uint8_t *)bus,
        .avcc_size = clip->config.size,
        .instance = saved_codec[1],
    };

    /* Publish the test pool only for the synchronous configure call. The
     * firmware allocates its two workspaces from this slot. */
    write_word(STOCK_VIDEO_POOL, (uintptr_t)pool);
    int result = ((codec_configure_fn)(CODEC_CONFIGURE | 1u))(codec, &description);
    write_word(STOCK_VIDEO_POOL, saved_pool);
    checkpoint("ConfigRx", (uint32_t)result, codec[15]);
    uintptr_t first = read_word(0x08811800u);
    uintptr_t second = read_word(0x08811858u);
    if (result == 0)
        checkpoint("Buffers", first, second);
    const char *error = NULL;
    if (result != 0)
        error = "H.264 setup failed. See the diagnostic log.";
    else if (!workspace_valid(first, WORKSPACE_A_BYTES) ||
             !workspace_valid(second, WORKSPACE_B_BYTES))
        error = "H.264 setup returned invalid buffers.";
    else {
        for (unsigned i = 0; i < clip->count && !error; i++)
            error = submit_frame(pool, codec, &frames[i], &clip->samples[i], i, clip->native);
    }

    /* Configure can run its own cleanup on an error. */
    if (result == 0 || codec_has_resources(codec)) {
        checkpoint("Cleanup", codec[15], 0);
        ((codec_cleanup_fn)(CODEC_CLEANUP | 1u))(codec);
        checkpoint("CleanRx", codec[15], 0);
    }
    for (unsigned i = 1; i < 16; i++)
        codec[i] = saved_codec[i];
    for (unsigned i = 0; i < sizeof(saved_driver) / sizeof(saved_driver[0]); i++)
        write_word(driver_slots[i], saved_driver[i]);

    if (error)
        return error;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < clip->count; i++)
        if (frames[i].references)
            return "The decoder kept a frame reference after cleanup.";
    return NULL;
}

static const char *convert_decoded_frame(const h264_clip *clip, const decoder_frame *frame, unsigned index)
{
    if (clip->calibration) {
        if (clip->native) {
            if (!measure_native_layout(frame, clip->width, clip->height))
                return "Could not verify the native video buffer layout.";
        } else
            measure_chroma(frame);
        return NULL;
    }
    if (clip->fixture && !check_fixture(frame->y, index))
        return "A decoded frame differs from the test clip. See the log.";
    if (!clip->fixture) {
        const volatile uint8_t *luma = frame->y;
        unsigned n = 0;
        unsigned luma_bytes = clip->native ? native_luma_stride * clip->height : FRAME_SIDE * FRAME_SIDE;
        while (n < luma_bytes && luma[n] == EMPTY_LUMA)
            n++;
        checkpoint("Pixels", index + 1u, n < luma_bytes);
        if (n == luma_bytes)
            return "The decoder returned an empty frame.";
    }
#ifdef NANO_YOUTUBE_VIDEO
    uint32_t started = hb_time_uptime_us();
#endif
    if (clip->native)
        convert_frame(frame, clip->cropped_output + index * clip->width * clip->height,
                      0, clip->width, clip->height, false, true);
    else if (clip->cropped_output)
        convert_frame(frame, clip->cropped_output + index * FRAME_SIDE * CACHE_HEIGHT,
                      CACHE_ROWS_START, FRAME_SIDE, CACHE_HEIGHT, false, false);
    else
        convert_frame(frame, preview[index], 0, FRAME_SIDE, FRAME_SIDE, clip->fixture, false);
#ifdef NANO_YOUTUBE_VIDEO
    measure_time(&video_perf.convert, started);
#endif
    return NULL;
}

static const char *release_decode_pool(void *pool, const char *error)
{
    checkpoint("PoolFree", (uintptr_t)pool, 0);
    ((pool_release_fn)(VIDEO_POOL_RELEASE | 1u))(pool);
    if (!checkpoint("PoolDone", error == NULL, 0) && !error)
        return "Pool released, but the final checkpoint could not be saved.";
    return error;
}

static const char decoder_low_memory[] = "Not enough contiguous RAM to decode video.";

static const char *decode_to_planes(const h264_clip *clip, decoder_frame *frames, void **output_pool)
{
    if (!firmware_matches()) {
        checkpoint("BadFW", 0, 0);
        return "Needs nano firmware 1.1.2 (39A10023).";
    }
    uintptr_t owner = read_word(STOCK_VIDEO_OWNER);
    checkpoint("Stock", owner, read_word(STOCK_VIDEO_POOL));
    if (owner)
        return "Exit stock video playback before testing.";

    /* Native playback configures the same codec for full and partial groups.
       Leave the full playback workspace available even for the final IDR. */
    unsigned pool_bytes = clip->native ? (clip->calibration ? NATIVE_POOL_OVERHEAD + NATIVE_FRAME_BYTES
                                                            : NATIVE_POOL_BYTES) :
                          clip->count <= 1 ? 1024u * 1024u : POOL_BYTES;
    if (clip->service_audio) {
        uint32_t largest = hb_os_heap_largest();
        if (largest < pool_bytes + DECODER_HEAP_MARGIN) {
            checkpoint("VLowMem", pool_bytes, largest);
            return decoder_low_memory;
        }
    }
    if (!checkpoint("PoolNew", pool_bytes, 0))
        return "Could not save the pool creation checkpoint.";
    pool_create_fn create = (pool_create_fn)(VIDEO_POOL_CREATE | 1u);
    void *pool = create(pool_bytes, "VideoStream_H264_Pool_Test", 0);
    checkpoint("PoolRx", (uintptr_t)pool, 0);
    if (!pool)
        return "Stock video pool allocation failed.";
    if (((uintptr_t)pool & 3u) || !in_sdram((uintptr_t)pool, 0x17acu))
        return "Stock allocator returned an invalid pool.";

    memset(frames, 0, sizeof(*frames) * clip->count);
    const char *error = decode_frames(pool, clip, frames);
    if (error)
        return release_decode_pool(pool, error);
    *output_pool = pool;
    return NULL;
}

static const char *decode_clip(const h264_clip *clip)
{
    decoder_frame frames[VIDEO_H264_MAX_FRAMES];
    void *pool;
    const char *error = decode_to_planes(clip, frames, &pool);
    if (error)
        return error;
    for (unsigned i = 0; i < clip->count && !error; i++) {
        error = convert_decoded_frame(clip, &frames[i], i);
        if (clip->service_audio)
            clip->service_audio();
    }
    return release_decode_pool(pool, error);
}

static void close_test(lv_event_t *event)
{
    lv_msgbox_close(lv_event_get_user_data(event));
}

#ifdef HB_VIDEO_LAYER_TEST
static void motion_plane_addresses(const decoder_frame *frames, uint32_t planes[VIDEO_LAYER_TEST_FRAMES][2])
{
    for (unsigned i = 0; i < VIDEO_LAYER_TEST_FRAMES; i++) {
        const uint8_t *uv = native_chroma_format.cb_plane ? frames[i].cr : frames[i].cb;
        planes[i][0] = (uint32_t)(uintptr_t)frames[i].y & 0x7fffffffu;
        planes[i][1] = (uint32_t)(uintptr_t)uv & 0x7fffffffu;
    }
}

const char *hb_h264_test_motion(void **pool, uint32_t planes[VIDEO_LAYER_TEST_FRAMES][2])
{
    const h264_clip color = {.config = {native_avcc, sizeof(native_avcc)},
        .samples = {{native_idr, sizeof(native_idr)}}, .count = 1, .native = true,
        .calibration = true, .width = NATIVE_WIDTH, .height = NATIVE_HEIGHT};
    const h264_clip motion = {.config = {motion_avcc, sizeof(motion_avcc)},
        .samples = {{motion_frame_0, sizeof(motion_frame_0)}, {motion_frame_1, sizeof(motion_frame_1)},
                    {motion_frame_2, sizeof(motion_frame_2)}, {motion_frame_3, sizeof(motion_frame_3)}},
        .count = VIDEO_LAYER_TEST_FRAMES, .native = true, .width = NATIVE_WIDTH, .height = NATIVE_HEIGHT};
    decoder_frame frames[VIDEO_LAYER_TEST_FRAMES];
    void *workspace;
    if (hb_os_heap_largest() < NATIVE_POOL_BYTES + DECODER_HEAP_MARGIN)
        return decoder_low_memory;
    const char *error = decode_to_planes(&color, frames, &workspace);
    if (error)
        return error;
    bool nv12 = measure_native_layout(frames, color.width, color.height) &&
        native_luma_stride == NATIVE_WIDTH && native_chroma_format.stride == NATIVE_WIDTH &&
        native_chroma_format.step == 2 && native_chroma_format.cb_offset == 0 &&
        native_chroma_format.cr_offset == 1 &&
        native_chroma_format.cb_plane == native_chroma_format.cr_plane;
    error = release_decode_pool(workspace, nv12 ? NULL : "Decoder output does not match the NV12 display layout.");
    if (error)
        return error;
    error = decode_to_planes(&motion, frames, &workspace);
    if (error)
        return error;
    motion_plane_addresses(frames, planes);
    *pool = workspace;
    return NULL;
}

void hb_h264_test_motion_release(void *pool)
{
    if (pool)
        release_decode_pool(pool, NULL);
}

const char *hb_h264_test_native_frame(void (*copy)(const uint8_t *, const uint8_t *, const uint8_t *,
                                                   unsigned, unsigned, unsigned, unsigned))
{
    const h264_clip clip = {.config = {native_avcc, sizeof(native_avcc)},
        .samples = {{native_idr, sizeof(native_idr)}}, .count = 1, .native = true,
        .calibration = true, .width = NATIVE_WIDTH, .height = NATIVE_HEIGHT};
    if (hb_os_heap_largest() < NATIVE_POOL_OVERHEAD + NATIVE_FRAME_BYTES + DECODER_HEAP_MARGIN)
        return decoder_low_memory;
    void *pool;
    decoder_frame frame;
    const char *error = decode_to_planes(&clip, &frame, &pool);
    if (error)
        return error;
    if (measure_native_layout(&frame, clip.width, clip.height)) {
        const uint8_t *chroma[] = {frame.cb, frame.cr};
        copy(frame.y, chroma[native_chroma_format.cb_plane] + native_chroma_format.cb_offset,
             chroma[native_chroma_format.cr_plane] + native_chroma_format.cr_offset,
             native_luma_stride, native_chroma_format.stride, native_chroma_format.step,
             native_chroma_format.cb_plane == native_chroma_format.cr_plane);
    } else {
        error = "Could not identify the decoder's plane layout.";
    }
    return release_decode_pool(pool, error);
}
#endif

static void add_close_button(lv_obj_t *dialog)
{
    lv_obj_t *close = lv_msgbox_add_footer_button(dialog, "Close");
    lv_obj_add_event_cb(close, close_test, LV_EVENT_CLICKED, dialog);
}

static void clear_preview_dialog(lv_event_t *event)
{
    (void)event;
    preview_dialog = NULL;
}

static lv_obj_t *open_preview(const char *title)
{
    if (preview_dialog)
        return NULL;
    lv_obj_t *dialog = lv_msgbox_create(NULL);
    if (dialog) {
        preview_dialog = dialog;
        lv_obj_add_event_cb(dialog, clear_preview_dialog, LV_EVENT_DELETE, NULL);
        lv_obj_set_width(dialog, 220);
        lv_msgbox_add_title(dialog, title);
    }
    return dialog;
}


typedef struct {
    uint8_t bytes[VIDEO_H264_MAX_AVCC];
    unsigned length, bit;
} nal_reader;

static void init_reader(nal_reader *reader, const uint8_t *data, unsigned size)
{
    reader->length = 0;
    reader->bit = 0;
    unsigned zeros = 0;
    for (unsigned i = 0; i < size && reader->length < sizeof(reader->bytes); i++) {
        uint8_t byte = data[i];
        if (zeros >= 2 && byte == 3) {
            zeros = 0;
            continue;
        }
        reader->bytes[reader->length++] = byte;
        zeros = byte == 0 ? zeros + 1u : 0;
    }
}

static uint32_t read_bit(nal_reader *reader)
{
    unsigned bit = reader->bit++;
    if (bit / 8u >= reader->length)
        return UINT32_MAX;
    return (reader->bytes[bit / 8u] >> (7u - bit % 8u)) & 1u;
}

static uint32_t read_ue(nal_reader *reader)
{
    unsigned zeros = 0;
    uint32_t bit;
    while ((bit = read_bit(reader)) == 0) {
        if (++zeros >= 16)
            return UINT32_MAX;
    }
    if (bit == UINT32_MAX)
        return UINT32_MAX;
    uint32_t value = 1;
    for (unsigned i = 0; i < zeros; i++) {
        bit = read_bit(reader);
        if (bit == UINT32_MAX)
            return UINT32_MAX;
        value = (value << 1) | bit;
    }
    return value - 1u;
}

/* Select buffers only for known progressive Baseline frame dimensions. */
static bool valid_config(const uint8_t *data, uint32_t size, const video_mode **mode)
{
    if (size < 14 || size > VIDEO_H264_MAX_AVCC || data[0] != 1 || data[1] != 66 ||
        data[3] > 30 || data[4] != 255 || data[5] != 225)
        return false;
    unsigned sps_size = ((unsigned)data[6] << 8) | data[7];
    if (sps_size < 5 || sps_size > size - 12u || (data[8] & 0x9fu) != 7 ||
        data[9] != 66 || data[11] > 30)
        return false;
    const uint8_t *pps = data + 8 + sps_size;
    unsigned pps_size = ((unsigned)pps[1] << 8) | pps[2];
    if (pps[0] != 1 || pps_size < 2 || pps_size != size - 11u - sps_size ||
        (pps[3] & 0x9fu) != 8)
        return false;

    nal_reader reader;
    init_reader(&reader, data + 12, sps_size - 4);
    if (read_ue(&reader) != 0 || read_ue(&reader) > 12)
        return false;
    uint32_t order = read_ue(&reader);
    if (order == 0) {
        if (read_ue(&reader) > 12)
            return false;
    } else if (order != 2) {
        return false;
    }
    uint32_t references = read_ue(&reader);
    if (!references || references > 4 || read_bit(&reader) != 0)
        return false;
    uint32_t width_mbs = read_ue(&reader), height_mbs = read_ue(&reader);
    const video_mode *selected = NULL;
    for (unsigned i = 0; i < sizeof(video_modes) / sizeof(video_modes[0]); i++) {
        if (width_mbs == video_modes[i].width / 16u - 1u && height_mbs == video_modes[i].height / 16u - 1u)
            selected = &video_modes[i];
    }
    if ((selected ? !mode || references != 1 : width_mbs != 7 || height_mbs != 7) ||
        read_bit(&reader) != 1 || read_bit(&reader) == UINT32_MAX ||
        read_bit(&reader) != 0)
        return false;

    init_reader(&reader, pps + 4, pps_size - 1);
    if (read_ue(&reader) != 0 || read_ue(&reader) != 0 || read_bit(&reader) != 0)
        return false;
    if (mode)
        *mode = selected;
    return true;
}

static bool valid_sample(const uint8_t *data, uint32_t size, unsigned index)
{
    if (size < 6 || size > VIDEO_H264_MAX_SAMPLE)
        return false;
    uint32_t nal_size = (uint32_t)data[0] << 24 | (uint32_t)data[1] << 16 |
                        (uint32_t)data[2] << 8 | data[3];
    unsigned nal_type = data[4] & 0x9fu;
    if (nal_size != size - 4 || (nal_type != 5 && (index == 0 || nal_type != 1)))
        return false;
    nal_reader reader;
    init_reader(&reader, data + 5, size - 5);
    if (read_ue(&reader) != 0)
        return false;
    uint32_t type = read_ue(&reader);
    if (type > 9 || (type % 5u != 0 && type % 5u != 2) || read_ue(&reader) != 0)
        return false;
    return nal_type != 5 || type % 5u == 2;
}

static uint32_t little_word(const uint8_t *data)
{
    return data[0] | (uint32_t)data[1] << 8 | (uint32_t)data[2] << 16 |
           (uint32_t)data[3] << 24;
}

#ifdef HB_VIDEO_LAYER_TEST
const char *hb_h264_test_decode_group(const uint8_t *config, uint32_t config_size,
                                      const uint8_t *group, uint32_t length, void **pool,
                                      uint32_t planes[VIDEO_LAYER_TEST_FRAMES][2])
{
    const video_mode *mode;
    if (!valid_config(config, config_size, &mode) || !mode || mode->width != NATIVE_WIDTH ||
        native_luma_stride != NATIVE_WIDTH)
        return "Unsupported test clip configuration.";
    h264_clip clip = {.config = {config, config_size}, .count = VIDEO_LAYER_TEST_FRAMES,
        .native = true, .width = NATIVE_WIDTH, .height = NATIVE_HEIGHT};
    for (unsigned i = 0; i < clip.count; i++) {
        if (length < 4u)
            return "Incomplete test frame.";
        uint32_t size = little_word(group);
        group += 4;
        length -= 4;
        if (!size || size > length || size > VIDEO_H264_MAX_SAMPLE || !valid_sample(group, size, i))
            return "Invalid test frame.";
        clip.samples[i] = (h264_sample){group, size};
        group += size;
        length -= size;
    }
    if (length)
        return "Unexpected bytes after the test group.";
    if (hb_os_heap_largest() < NATIVE_POOL_BYTES + DECODER_HEAP_MARGIN)
        return decoder_low_memory;
    decoder_frame frames[VIDEO_LAYER_TEST_FRAMES];
    const char *error = decode_to_planes(&clip, frames, pool);
    if (!error)
        motion_plane_addresses(frames, planes);
    return error;
}
#endif

typedef struct {
    uint32_t config_size, video_bytes, audio_bytes;
    unsigned audio_channels, audio_output_rate, audio_repeat;
    unsigned count, fps;
} h264_movie;

static bool read_media(void *data, uint32_t length)
{
    return hb_fs_media_reader_read(data, length) == length;
}

static bool parse_movie(uint32_t length, h264_movie *movie, uint8_t *config)
{
    uint8_t data[28];
    if (length < 12 || !read_media(data, 12))
        return false;
    bool has_audio_format = !memcmp(data, "NVH5", 4);
    bool has_audio_field = has_audio_format || !memcmp(data, "NVH4", 4);
    bool grouped = has_audio_field || !memcmp(data, "NVH3", 4);
    bool has_fps = grouped || !memcmp(data, "NVH2", 4);
    uint32_t header = has_audio_format ? 28 : has_audio_field ? 20 : has_fps ? 16 : 12;
    if (length < header || (!has_fps && memcmp(data, "NVH1", 4)) ||
        (header > 12 && !read_media(data + 12, header - 12)))
        return false;
    movie->fps = has_fps ? little_word(data + 12) : DEFAULT_FPS;
    if (!hb_video_frame_rate_supported(movie->fps))
        return false;
    movie->count = little_word(data + 4);
    /* The filesystem uses 32-bit offsets for the decoded frame cache. */
    unsigned max_cached_frames = UINT32_MAX / (FRAME_SIDE * (FRAME_SIDE * 9u / 16u) * sizeof(uint16_t));
    unsigned limit = grouped ? max_cached_frames : VIDEO_H264_MAX_FRAMES;
    uint32_t config_size = little_word(data + 8);
    if (!movie->count || movie->count > limit ||
        !config_size || config_size > VIDEO_H264_MAX_AVCC || config_size > length - header ||
        !read_media(config, config_size) || !valid_config(config, config_size, NULL))
        return false;
    movie->audio_bytes = has_audio_field ? little_word(data + 16) : 0;
    unsigned audio_rate = has_audio_format ? little_word(data + 20) : VIDEO_H264_AUDIO_RATE;
    movie->audio_channels = has_audio_format ? little_word(data + 24) : 1;
    if ((audio_rate != 7350 && audio_rate != 11025 && audio_rate != 22050 && audio_rate != 44100) ||
        (movie->audio_channels != 1 && movie->audio_channels != 2))
        return false;
    movie->audio_output_rate = audio_rate == 44100 ? 44100 : 22050;
    movie->audio_repeat = movie->audio_output_rate / audio_rate;
    if (movie->audio_bytes &&
        movie->audio_bytes != (uint64_t)movie->count * audio_rate / movie->fps * movie->audio_channels)
        return false;
    uint32_t remaining = length - header - config_size;
    if (movie->audio_bytes >= remaining)
        return false;
    movie->config_size = config_size;
    movie->video_bytes = remaining - movie->audio_bytes;
    return true;
}

#define CACHE_PATH "/Data/.VideoStream-frames.tmp"
#define CACHE_AHEAD 6u

static uint16_t (*frame_cache)[FRAME_SIDE * CACHE_HEIGHT];

#ifdef NANO_VIDEO_STREAM
static lv_obj_t *video_controls, *video_pause_label, *video_clock;
static bool video_paused;
static uint32_t video_paused_at;
static void toggle_video_pause(lv_event_t *event);
static void close_movie(lv_event_t *event);
#ifdef HB_VIDEO_LAYER
static void update_layer_visibility(void);
#endif

static void toggle_video_controls(lv_event_t *event)
{
    (void)event;
    if (!video_controls || video_paused)
        return;
    if (lv_obj_has_flag(video_controls, LV_OBJ_FLAG_HIDDEN))
        lv_obj_remove_flag(video_controls, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_add_flag(video_controls, LV_OBJ_FLAG_HIDDEN);
#ifdef HB_VIDEO_LAYER
    update_layer_visibility();
#endif
}

static void update_video_controls(double seconds)
{
    if (!video_controls || lv_obj_has_flag(video_controls, LV_OBJ_FLAG_HIDDEN))
        return;
    unsigned elapsed = (unsigned)seconds;
    char time[20];
    lv_snprintf(time, sizeof(time), "%u:%02u", elapsed / 60u, elapsed % 60u);
    if (strcmp(lv_label_get_text(video_clock), time))
        lv_label_set_text(video_clock, time);
}

static void video_controls_deleted(lv_event_t *event)
{
    (void)event;
    video_controls = video_pause_label = video_clock = NULL;
    video_paused = false;
}

static lv_obj_t *open_movie_canvas(bool direct_display)
{
    lv_obj_t *canvas;
    if (direct_display) {
        canvas = lv_obj_create(lv_layer_top());
        if (!canvas)
            return NULL;
        lv_obj_remove_style_all(canvas);
        lv_obj_set_size(canvas, LV_PCT(100), LV_PCT(100));
        lv_obj_set_style_bg_color(canvas, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(canvas, LV_OPA_COVER, 0);
        lv_obj_add_flag(canvas, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(canvas, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_event_cb(canvas, toggle_video_controls, LV_EVENT_CLICKED, NULL);
    } else {
        canvas = video_landscape_open(toggle_video_controls);
    }
    if (!canvas)
        return NULL;
    video_controls = lv_obj_create(canvas);
    lv_obj_remove_style_all(video_controls);
    lv_obj_remove_flag(video_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_all(video_controls, 0, 0);
    lv_obj_set_style_bg_color(video_controls, lv_color_hex(0x15171a), 0);
    lv_obj_set_style_bg_opa(video_controls, LV_OPA_90, 0);
    /* The video is rotated inside a portrait canvas. Keep the button hit
     * areas in screen coordinates and rotate only their text. */
    lv_obj_set_size(video_controls, 52, 280);
    lv_obj_align(video_controls, LV_ALIGN_LEFT_MID, 8, 0);
    lv_obj_add_event_cb(video_controls, video_controls_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *pause = lv_button_create(video_controls);
    lv_obj_set_size(pause, 40, 86);
    lv_obj_align(pause, LV_ALIGN_TOP_MID, 0, 6);
    lv_obj_add_event_cb(pause, toggle_video_pause, LV_EVENT_CLICKED, NULL);
    video_pause_label = lv_label_create(pause);
    lv_label_set_text(video_pause_label, "Pause");
    lv_obj_set_style_transform_pivot_x(video_pause_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(video_pause_label, LV_PCT(50), 0);
    lv_obj_set_style_transform_rotation(video_pause_label, 900, 0);
    lv_obj_center(video_pause_label);

    video_clock = lv_label_create(video_controls);
    lv_label_set_text(video_clock, "0:00");
    lv_obj_set_style_transform_pivot_x(video_clock, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(video_clock, LV_PCT(50), 0);
    lv_obj_set_style_transform_rotation(video_clock, 900, 0);
    lv_obj_center(video_clock);

    lv_obj_t *stop = lv_button_create(video_controls);
    lv_obj_set_size(stop, 40, 86);
    lv_obj_align(stop, LV_ALIGN_BOTTOM_MID, 0, -6);
    lv_obj_add_event_cb(stop, close_movie, LV_EVENT_CLICKED, canvas);
    lv_obj_t *label = lv_label_create(stop);
    lv_label_set_text(label, "Stop");
    lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(label, LV_PCT(50), 0);
    lv_obj_set_style_transform_rotation(label, 900, 0);
    lv_obj_center(label);
    lv_obj_add_flag(video_controls, LV_OBJ_FLAG_HIDDEN);
    return canvas;
}
#endif

static struct {
    h264_movie movie;
    uint8_t config[VIDEO_H264_MAX_AVCC];
    uint8_t (*samples)[VIDEO_H264_MAX_SAMPLE];
    uint8_t audio[4096];
    uint32_t started, audio_cursor, audio_loaded, audio_start;
    unsigned remaining, loaded, shown;
    bool cache_owned, writing, reading, media_open, remove_received, audio_started, audio_error, awake;
    lv_obj_t *canvas, *preparing, *status;
    lv_timer_t *timer;
} playback;

static void release_playback(void)
{
    if (playback.timer)
        lv_timer_delete(playback.timer);
    if (playback.audio_started)
        en_audio_shutdown();
    if (playback.writing)
        hb_fs_stream_close();
    if (playback.reading)
        hb_fs_reader_close();
    if (playback.cache_owned)
        hb_fs_remove(CACHE_PATH);
    if (playback.awake)
        hb_wake_lock(false);
    if (playback.media_open) {
        hb_fs_media_reader_close();
        if (playback.remove_received)
            hb_fs_remove(VIDEO_H264_RECEIVE_PATH);
    }
    hb_os_free(playback.samples);
    hb_os_free(preview);
    hb_os_free(frame_cache);
    playback.samples = NULL;
    preview = NULL;
    frame_cache = NULL;
    playback.timer = NULL;
    playback.canvas = playback.preparing = playback.status = NULL;
    playback.cache_owned = playback.writing = playback.reading = playback.media_open = false;
    playback.remove_received = false;
    playback.audio_started = playback.audio_error = playback.awake = false;
    playback.loaded = playback.shown = 0;
    playback.audio_cursor = playback.audio_loaded = playback.audio_start = 0;
}

static void playback_deleted(lv_event_t *event)
{
    (void)event;
    release_playback();
}

static void preparation_deleted(lv_event_t *event)
{
    if (playback.preparing == lv_event_get_target(event))
        release_playback();
}

static bool stream_open(void);

bool hb_h264_stop(void)
{
    if (stream_open())
        hb_h264_live_stop();
    if (playback.canvas)
        lv_obj_delete(playback.canvas);
    if (preview_dialog)
        lv_msgbox_close(preview_dialog);
    return !stream_open();
}

static void close_movie(lv_event_t *event)
{
#ifdef NANO_VIDEO_STREAM
    lv_obj_delete(lv_event_get_user_data(event));
#else
    lv_obj_delete(lv_event_get_target(event));
#endif
}

#ifdef NANO_VIDEO_STREAM
static void toggle_video_pause(lv_event_t *event)
{
    (void)event;
    video_paused = !video_paused;
    if (playback.canvas && !playback.audio_started) {
        uint32_t now = lv_tick_get();
        if (video_paused)
            video_paused_at = now;
        else
            playback.started += now - video_paused_at;
    } else {
        en_audio_set_stream_paused(video_paused);
    }
    lv_label_set_text(video_pause_label, video_paused ? "Resume" : "Pause");
}
#else
static lv_obj_t *open_movie_canvas(bool direct_display)
{
    (void)direct_display;
    return video_landscape_open(close_movie);
}
#endif

static const char *decode_next_batch(void)
{
    unsigned count = playback.remaining;
    if (count > VIDEO_H264_MAX_FRAMES)
        count = VIDEO_H264_MAX_FRAMES;
    h264_clip clip = {.config = {playback.config, playback.movie.config_size}, .count = count,
                      .cropped_output = &preview[0][0]};
    for (unsigned i = 0; i < count; i++) {
        uint8_t size_bytes[4];
        if (playback.movie.video_bytes < sizeof(size_bytes) || !read_media(size_bytes, sizeof(size_bytes)))
            return "Could not read the video frame header.";
        uint32_t size = little_word(size_bytes);
        playback.movie.video_bytes -= sizeof(size_bytes);
        if (!size || size > VIDEO_H264_MAX_SAMPLE || size > playback.movie.video_bytes ||
            !read_media(playback.samples[i], size) || !valid_sample(playback.samples[i], size, i))
            return "Invalid or unreadable video frame.";
        clip.samples[i] = (h264_sample){playback.samples[i], size};
        playback.movie.video_bytes -= size;
    }
    if (count == playback.remaining && playback.movie.video_bytes)
        return "Unexpected data after the video frames.";

    unsigned first = playback.movie.count - playback.remaining;
    checkpoint("Batch", first, count);
    const char *error = decode_clip(&clip);
    if (error) {
        disk_checkpoints = true;
        checkpoint("BatchErr", first, count);
        return error;
    }
    playback.remaining -= count;
    if (!hb_fs_stream_write(preview, count * sizeof(frame_cache[0])))
        return "Could not write the temporary video frames.";
    disk_checkpoints = false;
    return NULL;
}

static bool read_cached_frame(void)
{
    uint16_t *frame = frame_cache[playback.loaded % CACHE_AHEAD];
    if (hb_fs_reader_read(frame, sizeof(frame_cache[0])) != sizeof(frame_cache[0]))
        return false;
    playback.loaded++;
    return true;
}

static void present_cached_frame(unsigned index)
{
    playback.shown = index;
    video_landscape_present_cropped(playback.canvas, frame_cache[index % CACHE_AHEAD],
                                    FRAME_SIDE, CACHE_HEIGHT);
}

static uint32_t pull_movie_audio(int16_t *pcm, uint32_t frames, uint32_t advance, void *context)
{
    (void)advance;
    (void)context;
    unsigned channels = playback.movie.audio_channels;
    unsigned repeat = playback.movie.audio_repeat;
    uint32_t available = playback.movie.audio_bytes / channels * repeat - playback.audio_cursor;
    if (frames > available)
        frames = available;
    for (uint32_t i = 0; i < frames; i++) {
        uint32_t index = playback.audio_cursor / repeat * channels;
        if (index >= playback.audio_loaded) {
            uint32_t count = playback.movie.audio_bytes - index;
            if (count > sizeof(playback.audio))
                count = sizeof(playback.audio);
            if (!read_media(playback.audio, count)) {
                playback.audio_error = true;
                return i;
            }
            playback.audio_start = index;
            playback.audio_loaded = index + count;
        }
        const uint8_t *sample = playback.audio + index - playback.audio_start;
        int16_t left = hb_mulaw_decode(sample[0]);
        *pcm++ = left;
        *pcm++ = channels == 2 ? hb_mulaw_decode(sample[1]) : left;
        playback.audio_cursor++;
    }
    return frames;
}

static void movie_failed(const char *error)
{
    if (playback.canvas)
        lv_obj_delete(playback.canvas);
    else if (playback.preparing)
        lv_msgbox_close(playback.preparing);
    else
        release_playback();
    lv_obj_t *dialog = open_preview("Video playback");
    if (dialog) {
        lv_msgbox_add_text(dialog, error);
        add_close_button(dialog);
    }
}

static const char *start_cached_movie(void)
{
    hb_os_free(playback.samples);
    hb_os_free(preview);
    playback.samples = NULL;
    preview = NULL;
    bool closed = hb_fs_stream_close();
    playback.writing = false;
    if (!closed || !hb_fs_reader_open(CACHE_PATH))
        return "Could not open the temporary video frames.";
    playback.reading = true;
    unsigned preload = playback.movie.count < CACHE_AHEAD ? playback.movie.count : CACHE_AHEAD;
    uint32_t initial_bytes = preload * sizeof(frame_cache[0]);
    if (hb_os_heap_largest() >= initial_bytes + PLAYBACK_HEAP_MARGIN)
        frame_cache = hb_os_alloc(initial_bytes);
    if (!frame_cache)
        return "Not enough memory to read the temporary video frames.";
    if (hb_fs_reader_read(frame_cache, initial_bytes) != initial_bytes)
        return "Could not read the temporary video frames.";
    playback.loaded = preload;

    lv_obj_t *dialog = playback.preparing;
    playback.preparing = NULL;
    lv_msgbox_close(dialog);
    playback.canvas = open_movie_canvas(false);
    if (!playback.canvas)
        return "Could not open the full-screen video player.";
    lv_obj_add_event_cb(playback.canvas, playback_deleted, LV_EVENT_DELETE, NULL);
    present_cached_frame(0);
    if (playback.movie.audio_bytes) {
        playback.audio_started = true;
        if (!en_audio_start_buffered_stream(playback.movie.audio_output_rate, pull_movie_audio, NULL))
            return "Could not start video audio.";
    }
    playback.started = lv_tick_get();
    return NULL;
}

static void advance_movie(lv_timer_t *timer)
{
    (void)timer;
    if (playback.preparing) {
        const char *error = decode_next_batch();
        if (!error && !playback.remaining)
            error = start_cached_movie();
        if (error)
            movie_failed(error);
        else if (playback.preparing)
            lv_label_set_text_fmt(playback.status, "Preparing video... %u%%",
                (playback.movie.count - playback.remaining) * 100u / playback.movie.count);
        return;
    }

    unsigned next;
    if (playback.audio_started) {
        en_audio_tick();
        en_audio_state_t state = en_audio_state();
        if (state == EN_AUDIO_FAILED || playback.audio_error) {
            movie_failed("Video audio stopped unexpectedly.");
            return;
        }
        if (state == EN_AUDIO_IDLE) {
            lv_obj_delete(playback.canvas);
            return;
        }
        next = (unsigned)(en_audio_elapsed() * playback.movie.fps);
    } else {
        uint32_t elapsed = lv_tick_elaps(playback.started);
#ifdef NANO_VIDEO_STREAM
        if (video_paused)
            elapsed = video_paused_at - playback.started;
#endif
        next = (uint64_t)elapsed * playback.movie.fps / 1000u;
        if (next >= playback.movie.count) {
            lv_obj_delete(playback.canvas);
            return;
        }
    }
#ifdef NANO_VIDEO_STREAM
    update_video_controls((double)next / playback.movie.fps);
#endif

    unsigned ready = next < playback.loaded ? next : playback.loaded - 1u;
    if (ready > playback.shown)
        present_cached_frame(ready);
    if (playback.loaded < playback.movie.count && playback.loaded - playback.shown < CACHE_AHEAD &&
        !read_cached_frame())
        movie_failed("Could not read the next video frame.");
}

bool hb_h264_can_receive(void)
{
    return !stream_open() && !preview_dialog && !playback.canvas && !playback.preparing;
}

static const char *play_file(const char *path, uint32_t length, bool remove_received)
{
    if (!hb_h264_can_receive())
        return "Close the current video first.";
    if (!hb_fs_media_reader_open(path)) {
        if (remove_received)
            hb_fs_remove(VIDEO_H264_RECEIVE_PATH);
        return "Could not open the video.";
    }
    playback.media_open = true;
    playback.remove_received = remove_received;
    h264_movie movie = {0};
    if (!parse_movie(length, &movie, playback.config)) {
        release_playback();
        return "Unsupported hardware clip. Check the Mac sender.";
    }
    playback.samples = hb_os_alloc(VIDEO_H264_MAX_FRAMES * sizeof(*playback.samples));
    preview = hb_os_alloc(VIDEO_H264_MAX_FRAMES * sizeof(*preview));
    if (!playback.samples || !preview) {
        release_playback();
        return "Not enough free memory to decode the video.";
    }
    playback.movie = movie;
    playback.remaining = movie.count;

    log_path = "/Data/VideoStream-h264-mp4.txt";
    log_used = 0;
    disk_checkpoints = true;
    const char *error;
    if (!(hb_fs_is_dir("/Data") || hb_fs_mkdir("/Data")) ||
        !checkpoint("MacClip", movie.count, length)) {
        error = "Could not save the diagnostic log.";
    } else {
        if (!chroma_format.stride) {
            const h264_clip color = {.config = {color_avcc, sizeof(color_avcc)},
                .samples = {{color_idr, sizeof(color_idr)}}, .count = 1, .calibration = true};
            checkpoint("Color", 0, 0);
            if (decode_clip(&color))
                checkpoint("ColorErr", 0, 0);
        }
        checkpoint("Rate", movie.fps, movie.count);
        error = NULL;
    }

    if (error) {
        release_playback();
        return error;
    }

    if (!hb_fs_stream_open(CACHE_PATH)) {
        release_playback();
        return "Could not create the temporary video cache.";
    }
    playback.cache_owned = playback.writing = true;
    playback.preparing = open_preview("H.264 video");
    if (!playback.preparing) {
        release_playback();
        return "Could not open the video preparation screen.";
    }
    lv_obj_add_event_cb(playback.preparing, preparation_deleted, LV_EVENT_DELETE, NULL);
    playback.status = lv_msgbox_add_text(playback.preparing, "Preparing video... 0%");
    add_close_button(playback.preparing);
    playback.timer = lv_timer_create(advance_movie, 16, NULL);
    if (!playback.timer) {
        lv_msgbox_close(playback.preparing);
        return "Could not start the video player.";
    }
    hb_wake_lock(true);
    playback.awake = true;
    return NULL;
}

const char *hb_h264_play_file(const char *path)
{
    if (!path || !*path)
        return "Choose a video file.";
    if (!strcmp(path, CACHE_PATH))
        return "Choose a file outside the video cache.";
    if (!hb_h264_init())
        return "Video playback requires the NanoApps runtime.";
    return play_file(path, hb_fs_size(path), false);
}

const char *hb_h264_play_received(uint32_t length)
{
    return play_file(VIDEO_H264_RECEIVE_PATH, length, true);
}

#define LIVE_GROUPS 6u
#define MAX_VIDEO_GROUPS 12u

static struct {
    uint8_t config[VIDEO_H264_MAX_AVCC];
    uint16_t *pixels[MAX_VIDEO_GROUPS];
#ifdef HB_VIDEO_LAYER
    void *nv12_allocations[MAX_VIDEO_GROUPS];
    void *scanout_allocation;
    unsigned nv12_span, nv12_uv_offset, scanout_index;
    uint16_t scale_x[NATIVE_WIDTH], scale_y[NATIVE_HEIGHT];
    uint16_t scale_uv_x[NATIVE_WIDTH / 2], scale_uv_y[NATIVE_HEIGHT / 2];
    uint32_t ui_refresh_at;
#endif
    uint8_t *audio, *disk_group;
    uint8_t (*samples)[VIDEO_H264_MAX_SAMPLE];
    void *pending_pool;
    h264_clip pending_clip;
    decoder_frame pending_frames[VIDEO_H264_MAX_FRAMES];
    unsigned converted;
    uint32_t pending_decode_us;
    lv_obj_t *canvas;
    uint32_t count, fps, rate, channels, repeat, config_size, audio_slot_size;
    unsigned width, height, groups, group_frames;
    uint32_t loaded, audio_group;
    uint64_t audio_frame, audio_group_start, audio_group_end;
    uint32_t ima_sample;
    int ima_predictor[2], ima_index[2];
    unsigned shown;
    bool started, cancelled, from_disk, awake, native, adpcm, audio_eof_logged;
    bool direct_display, closing, refresh_pending;
    uint32_t refresh_blocked_at;
    const char *end_reason;
    lv_timer_t *timer;
} stream;

static const char *live_playback_error;
static void advance_spooled_stream(lv_timer_t *timer);

#ifdef HB_VIDEO_LAYER
static uint8_t *dma_buffer(void *allocation)
{
    uintptr_t raw = (uintptr_t)allocation;
    return (uint8_t *)((((raw + DMA_ALIGNMENT - 1u) & ~(uintptr_t)(DMA_ALIGNMENT - 1u))) | 0x80000000u);
}

static void init_scale_indices(uint16_t *indices, unsigned source, unsigned target)
{
    for (unsigned i = 0; i < target; i++)
        indices[i] = i * source / target;
}

static void copy_nv12_frame(const decoder_frame *frame, unsigned index)
{
    unsigned slot = stream.loaded / stream.group_frames % stream.groups;
    uint8_t *dst = dma_buffer(stream.nv12_allocations[slot]) + index * stream.nv12_span;
    const uint8_t *planes[] = {frame->cb, frame->cr};
    unsigned width = stream.width, height = stream.height;
    unsigned first_row = stream.native ? 0 : CACHE_ROWS_START;
    unsigned pitch = stream.native ? native_luma_stride : FRAME_SIDE;
    chroma_layout chroma = stream.native ? native_chroma_format : chroma_format;
    const volatile uint8_t *y = frame->y + first_row * pitch;
    const volatile uint8_t *cb = planes[chroma.cb_plane] + first_row / 2u * chroma.stride;
    const volatile uint8_t *cr = planes[chroma.cr_plane] + first_row / 2u * chroma.stride;
    bool interleaved = cb == cr && chroma.step == 2 && !chroma.cb_offset && chroma.cr_offset == 1;
    if (pitch == width)
        copy_dma_words((uint32_t *)dst, y, width * height);
    else
        for (unsigned row = 0; row < height; row++)
            copy_dma_words((uint32_t *)(dst + row * width), y + row * pitch, width);
    dst += stream.nv12_uv_offset;
    if (interleaved && chroma.stride == width) {
        copy_dma_words((uint32_t *)dst, cb, width * height / 2u);
    } else {
        for (unsigned row = 0; row < height / 2u; row++) {
            if (interleaved) {
                copy_dma_words((uint32_t *)dst, cb, width);
            } else {
                volatile uint32_t *words = (volatile uint32_t *)dst;
                uint32_t cb_row[NATIVE_WIDTH / 4u], cr_row[NATIVE_WIDTH / 4u];
                unsigned bytes = width / 2u * chroma.step;
                copy_dma_words(cb_row, cb, bytes);
                if (cr != cb)
                    copy_dma_words(cr_row, cr, bytes);
                const uint8_t *u = (const uint8_t *)cb_row + chroma.cb_offset;
                const uint8_t *v = (const uint8_t *)(cr == cb ? cb_row : cr_row) + chroma.cr_offset;
                for (unsigned x = 0; x < width / 4u; x++) {
                    unsigned p = 2u * x * chroma.step;
                    words[x] = u[p] | (uint32_t)v[p] << 8 |
                               (uint32_t)u[p + chroma.step] << 16 | (uint32_t)v[p + chroma.step] << 24;
                }
            }
            cb += chroma.stride;
            cr += chroma.stride;
            dst += width;
        }
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

/* Preserve the panel geometry verified on hardware. Scale only frames that
 * reach the display, with two separate buffers for scanout and preparation. */
static void scale_nv12_plane(uint8_t *dst, const uint8_t *src, unsigned pitch,
                             unsigned rows, const uint16_t *columns, const uint16_t *source_rows,
                             bool uv)
{
    uint32_t input[NATIVE_WIDTH / 4u], output[NATIVE_WIDTH / 4u];
    unsigned previous = UINT32_MAX;
    for (unsigned row = 0; row < rows; row++) {
        unsigned y = source_rows[row];
        if (y != previous) {
            copy_dma_words(input, src + y * pitch, pitch);
            const uint8_t *bytes = (const uint8_t *)input;
            const uint16_t *pairs = (const uint16_t *)input;
            for (unsigned x = 0; x < NATIVE_WIDTH / 4u; x++) {
                if (uv)
                    output[x] = pairs[columns[2u * x]] | (uint32_t)pairs[columns[2u * x + 1u]] << 16;
                else
                    output[x] = bytes[columns[4u * x]] | (uint32_t)bytes[columns[4u * x + 1u]] << 8 |
                                (uint32_t)bytes[columns[4u * x + 2u]] << 16 | (uint32_t)bytes[columns[4u * x + 3u]] << 24;
            }
            previous = y;
        }
        volatile uint32_t *write = (volatile uint32_t *)dst;
        for (unsigned x = 0; x < NATIVE_WIDTH / 4u; x++)
            write[x] = output[x];
        dst += NATIVE_WIDTH;
    }
}

static bool present_nv12_frame(unsigned slot, unsigned index)
{
    if (!hb_video_layer_idle())
        return false;
    uint8_t *src = dma_buffer(stream.nv12_allocations[slot]) + index * stream.nv12_span;
    uint8_t *dst = src;
    if (stream.scanout_allocation) {
        dst = dma_buffer(stream.scanout_allocation) + stream.scanout_index * NV12_FRAME_SPAN;
        scale_nv12_plane(dst, src, stream.width, NATIVE_HEIGHT, stream.scale_x, stream.scale_y, false);
        scale_nv12_plane(dst + NV12_Y_SPAN, src + stream.nv12_uv_offset, stream.width,
                         NATIVE_HEIGHT / 2u, stream.scale_uv_x, stream.scale_uv_y, true);
    }
    uint32_t y = (uint32_t)(uintptr_t)dst & 0x7fffffffu;
    if (!hb_video_layer_present(y, y + NV12_Y_SPAN))
        return false;
    stream.scanout_index ^= 1u;
    return true;
}

#ifdef NANO_VIDEO_STREAM
static void update_layer_visibility(void)
{
    if (stream.direct_display)
        stream.refresh_pending = true;
}
#endif
#endif

/* The RAM trace can disappear on reboot. Save the last completed stage of
   saved-video playback, with no writes during steady-state playback. */
static void log_saved_video_end(const char *stage)
{
    if (!stream.from_disk)
        return;
    char line[224];
    unsigned queued, playing;
    en_audio_queue_status(&queued, &playing);
    int size = lv_snprintf(line, sizeof(line),
        "%s\n%u x %u, %u FPS, %u Hz, %u channels\n"
        "Decoded frames: %lu / %lu\nQueued audio blocks: %u, mixer flags: %x\n",
        stage, stream.width, stream.height, (unsigned)stream.fps,
        (unsigned)stream.rate, (unsigned)stream.channels,
        (unsigned long)stream.loaded, (unsigned long)stream.count, queued, playing);
    if (size > 0 && (size_t)size < sizeof(line))
        hb_fs_write("/Data/VideoStream-end.txt", line, (uint32_t)size);
}

static uint32_t group_audio_bytes(uint32_t frames, unsigned channels, bool adpcm)
{
    if (!frames || !adpcm)
        return frames * channels;
    return channels * (4u + 4u * ((frames - 1u + 7u) / 8u));
}

static const char *startup_failure(const char *error, const uint8_t *config, uint32_t size,
                                   uint32_t fps, uint32_t rate, uint32_t channels)
{
    char report[400];
    unsigned used = log_used;
    const char *path = "/Data/NativeVideo-decoder.txt";
    int length = lv_snprintf(report, sizeof(report),
        "%s\nVideo: %u x %u at %u FPS  Audio: %u Hz, %u channel(s)\n"
        "Decoded frames: %u / %u\n"
        "Free heap: %u  Largest block: %u bytes\nAVCC:",
        error, stream.width, stream.height, (unsigned)fps, (unsigned)rate, (unsigned)channels,
        (unsigned)stream.loaded, stream.groups * stream.group_frames,
        (unsigned)hb_os_heap_free(), (unsigned)hb_os_heap_largest());
    if (length > 0 && (unsigned)length < sizeof(report)) {
        for (unsigned i = 0; config && i < size && i < VIDEO_H264_MAX_AVCC; i++) {
            if ((unsigned)length + 4 >= sizeof(report))
                break;
            length += lv_snprintf(report + length, sizeof(report) - length, " %02X", (unsigned)config[i]);
        }
        report[length++] = '\n';
        if (hb_fs_is_dir("/Data") || hb_fs_mkdir("/Data")) {
            void *parts[] = {report, log_text};
            const uint32_t lengths[] = {(uint32_t)length, used};
            hb_fs_write_parts(path, parts, lengths, used ? 2 : 1);
        }
    }
    return error;
}

static bool begin_group_playback(const uint8_t *config, uint32_t size, uint32_t count,
                                 uint32_t fps, uint32_t rate, uint32_t channels,
                                 unsigned group_frames, bool from_disk)
{
    const video_mode *mode;
    live_playback_error = NULL;
    if (!hb_h264_can_receive()) {
        live_playback_error = "Close the current video first.";
        return false;
    }
    log_used = 0;
    if (!config || !valid_config(config, size, &mode)) {
        live_playback_error = startup_failure("Unsupported H.264 stream format.", config, size, fps, rate, channels);
        return false;
    }
    bool native = mode != NULL;
    bool adpcm = (channels & VIDEO_H264_AUDIO_ADPCM) != 0;
    channels &= ~VIDEO_H264_AUDIO_ADPCM;
    if (group_frames != (native ? VIDEO_H264_NATIVE_GROUP_FRAMES : VIDEO_H264_MAX_FRAMES)) {
        live_playback_error = startup_failure("Update the Mac sender and retry this video.", config, size, fps, rate, channels);
        return false;
    }
    if (!hb_video_frame_rate_supported(fps) ||
#ifndef HB_VIDEO_LAYER
        (native && mode->width == NATIVE_WIDTH && fps != 22 && fps != 30) ||
#endif
        (rate != 7350 && rate != 11025 && rate != 22050 && rate != 44100) ||
        (channels != 1 && channels != 2)) {
        live_playback_error = startup_failure("Unsupported video timing or audio format.", config, size, fps, rate, channels);
        return false;
    }
    stream.native = native;
    stream.adpcm = adpcm;
    stream.groups = native ? MAX_VIDEO_GROUPS : LIVE_GROUPS;
    stream.group_frames = group_frames;
    stream.width = native ? mode->width : FRAME_SIDE;
    stream.height = native ? mode->height : CACHE_HEIGHT;
    unsigned audio_slot_size = group_audio_bytes((group_frames * rate + fps - 1u) / fps,
                                                 channels, adpcm);
    unsigned sample_bytes = group_frames * sizeof(*stream.samples);
    unsigned disk_bytes = from_disk ? group_frames * 4u + sample_bytes + audio_slot_size : 0;
    static const video_mode *calibrated_mode;
    if (native && mode != calibrated_mode) {
        if (hb_os_heap_largest() < NATIVE_POOL_OVERHEAD + NATIVE_FRAME_BYTES + DECODER_HEAP_MARGIN) {
            live_playback_error = startup_failure(decoder_low_memory, config, size, fps, rate, channels);
            hb_h264_live_stop();
            return false;
        }
        const h264_clip color = {.config = mode->config, .samples = {mode->sample},
            .width = mode->width, .height = mode->height, .count = 1, .calibration = true, .native = true};
        capture_checkpoints = true;
        const char *error = decode_clip(&color);
        capture_checkpoints = false;
        if (error) {
            native_luma_stride = 0;
            calibrated_mode = NULL;
            live_playback_error = startup_failure(error, config, size, fps, rate, channels);
            hb_h264_live_stop();
            return false;
        }
        calibrated_mode = mode;
    }
#ifdef HB_VIDEO_LAYER
    if (!native && !chroma_format.stride) {
        const h264_clip color = {.config = {color_avcc, sizeof(color_avcc)},
            .samples = {{color_idr, sizeof(color_idr)}}, .count = 1, .calibration = true};
        decode_clip(&color);
    }
    stream.direct_display = (native || chroma_format.stride) && hb_video_layer_available();
#endif
    unsigned group_bytes = group_frames * stream.width * stream.height * sizeof(uint16_t);
#ifdef HB_VIDEO_LAYER
    if (stream.direct_display) {
        unsigned luma = stream.width * stream.height;
        stream.nv12_uv_offset = (luma + DMA_ALIGNMENT - 1u) & ~(DMA_ALIGNMENT - 1u);
        stream.nv12_span = (stream.nv12_uv_offset + luma / 2u + DMA_ALIGNMENT - 1u) & ~(DMA_ALIGNMENT - 1u);
        group_bytes = group_frames * stream.nv12_span + DMA_ALIGNMENT - 1u;
        if (stream.width != NATIVE_WIDTH) {
            unsigned bytes = 2u * NV12_FRAME_SPAN;
            if (hb_os_heap_largest() >= bytes + DMA_ALIGNMENT - 1u + DECODER_HEAP_MARGIN)
                stream.scanout_allocation = hb_os_alloc(bytes + DMA_ALIGNMENT - 1u);
            if (!stream.scanout_allocation ||
                !workspace_valid((uintptr_t)dma_buffer(stream.scanout_allocation), bytes)) {
                live_playback_error = startup_failure("Could not allocate the hardware video display buffers.",
                                                      config, size, fps, rate, channels);
                hb_h264_live_stop();
                return false;
            }
            init_scale_indices(stream.scale_x, stream.width, NATIVE_WIDTH);
            init_scale_indices(stream.scale_y, stream.height, NATIVE_HEIGHT);
            init_scale_indices(stream.scale_uv_x, stream.width / 2u, NATIVE_WIDTH / 2u);
            init_scale_indices(stream.scale_uv_y, stream.height / 2u, NATIVE_HEIGHT / 2u);
        }
    }
#endif
    unsigned pool_bytes = native ? NATIVE_POOL_BYTES : POOL_BYTES;
    unsigned reserved = pool_bytes + 1024u * 1024u + sample_bytes + disk_bytes;
    bool guard_decoder_heap = native || stream.direct_display;
    if (guard_decoder_heap) {
        unsigned free = hb_os_heap_free(), largest = hb_os_heap_largest();
        while (stream.groups > 2 && free < reserved + stream.groups * (group_bytes + audio_slot_size))
            stream.groups--;
        if (largest < group_bytes || free < reserved + stream.groups * (group_bytes + audio_slot_size)) {
            live_playback_error = startup_failure("Not enough free memory for this video resolution.", config, size, fps, rate, channels);
            hb_h264_live_stop();
            return false;
        }
    }
    /* Allocate the mixer's PCM before RGB frames. Allocating at the first
       display can consume part of the decoder's only large free region. */
    if (native && !en_audio_reserve_video_buffers()) {
        live_playback_error = startup_failure("Not enough memory for video audio.", config, size, fps, rate, channels);
        hb_h264_live_stop();
        return false;
    }
    stream.audio_slot_size = audio_slot_size;
    stream.audio = hb_os_alloc(stream.groups * audio_slot_size);
    stream.samples = hb_os_alloc(sample_bytes);
    if (from_disk)
        stream.disk_group = hb_os_alloc(disk_bytes);
    if (!stream.audio || !stream.samples || (from_disk && !stream.disk_group)) {
        live_playback_error = startup_failure("Could not allocate video and audio buffers.", config, size, fps, rate, channels);
        hb_h264_live_stop();
        return false;
    }
    for (unsigned i = 0; i < stream.groups; i++) {
        if (guard_decoder_heap && hb_os_heap_largest() < group_bytes + DECODER_HEAP_MARGIN) {
            stream.groups = i;
            if (i >= 2)
                break;
            live_playback_error = startup_failure("Could not allocate decoded video frames.", config, size, fps, rate, channels);
            hb_h264_live_stop();
            return false;
        }
        void *buffer = hb_os_alloc(group_bytes);
        if (!buffer) {
            if (guard_decoder_heap && i >= 2) {
                stream.groups = i;
                break;
            }
            live_playback_error = startup_failure("Could not allocate decoded video frames.", config, size, fps, rate, channels);
            hb_h264_live_stop();
            return false;
        }
#ifdef HB_VIDEO_LAYER
        if (stream.direct_display) {
            stream.nv12_allocations[i] = buffer;
            if (!workspace_valid((uintptr_t)dma_buffer(buffer), group_frames * stream.nv12_span)) {
                live_playback_error = startup_failure("Could not allocate DMA video frames.", config, size, fps, rate, channels);
                hb_h264_live_stop();
                return false;
            }
        } else
#endif
            stream.pixels[i] = buffer;
    }
    /* Keep space for playback bookkeeping in addition to the decoder pool. */
    while (guard_decoder_heap && stream.groups > 2 &&
           hb_os_heap_largest() < pool_bytes + DECODER_HEAP_MARGIN + PLAYBACK_HEAP_MARGIN) {
        stream.groups--;
#ifdef HB_VIDEO_LAYER
        hb_os_free(stream.nv12_allocations[stream.groups]);
        stream.nv12_allocations[stream.groups] = NULL;
#endif
        hb_os_free(stream.pixels[stream.groups]);
        stream.pixels[stream.groups] = NULL;
    }
    if (guard_decoder_heap && hb_os_heap_largest() < pool_bytes + DECODER_HEAP_MARGIN) {
        checkpoint("VLowMem", pool_bytes, hb_os_heap_largest());
        live_playback_error = startup_failure(decoder_low_memory,
                                              config, size, fps, rate, channels);
        hb_h264_live_stop();
        return false;
    }
    memcpy(stream.config, config, size);
    stream.config_size = size;
    stream.count = count ? count : UINT32_MAX;
    stream.fps = fps;
    stream.rate = rate;
    stream.channels = channels;
    stream.repeat = (rate == 44100 ? 44100 : 22050) / rate;
    stream.shown = UINT32_MAX;
    log_path = "/Data/VideoStream-h264-live.txt";
    log_used = 0;
    disk_checkpoints = false;
    if (!native && !chroma_format.stride) {
        const h264_clip color = {.config = {color_avcc, sizeof(color_avcc)},
            .samples = {{color_idr, sizeof(color_idr)}}, .count = 1, .calibration = true};
        decode_clip(&color);
    }
    hb_wake_lock(true);
    stream.awake = true;
#ifdef NANO_YOUTUBE_VIDEO
    memset(&video_perf, 0, sizeof(video_perf));
    video_perf.active = true;
#endif
    return true;
}

bool hb_h264_live_begin(const uint8_t *config, uint32_t size, uint32_t count,
                           uint32_t fps, uint32_t rate, uint32_t channels, unsigned group_frames)
{
    return begin_group_playback(config, size, count, fps, rate, channels, group_frames, false);
}

static void live_canvas_deleted(lv_event_t *event)
{
    (void)event;
    stream.canvas = NULL;
    stream.cancelled = true;
}

void hb_h264_live_stop(void)
{
#ifdef HB_VIDEO_LAYER
    if (stream.direct_display && !hb_video_layer_close()) {
        stream.closing = stream.cancelled = true;
        if (stream.started) {
            en_audio_shutdown();
            stream.started = false;
        }
        if (!stream.timer)
            stream.timer = lv_timer_create(advance_spooled_stream, 8, NULL);
        return;
    }
#endif
    if (stream.timer)
        lv_timer_delete(stream.timer);
    if (stream.started)
        en_audio_shutdown();
    if (stream.pending_pool)
        release_decode_pool(stream.pending_pool, NULL);
    if (stream.from_disk)
        hb_fs_media_reader_close();
    if (stream.canvas)
        lv_obj_delete(stream.canvas);
    if (stream.awake)
        hb_wake_lock(false);
    for (unsigned i = 0; i < stream.groups; i++) {
        hb_os_free(stream.pixels[i]);
#ifdef HB_VIDEO_LAYER
        hb_os_free(stream.nv12_allocations[i]);
#endif
    }
    hb_os_free(stream.audio);
    hb_os_free(stream.samples);
    hb_os_free(stream.disk_group);
#ifdef HB_VIDEO_LAYER
    hb_os_free(stream.scanout_allocation);
#endif
    if (stream.from_disk) {
        log_saved_video_end(stream.end_reason ? stream.end_reason : "Playback stopped and buffers released");
        hb_fs_remove(VIDEO_H264_RECEIVE_PATH);
    }
#ifdef NANO_YOUTUBE_VIDEO
    if (video_perf.active) {
        video_perf.active = false;
        if (video_perf.group.calls)
            save_video_timings(stream.fps, stream.from_disk, stream.groups * stream.group_frames, stream.direct_display);
    }
#endif
    memset(&stream, 0, sizeof(stream));
}

static uint64_t live_audio_boundary(uint64_t frame)
{
    return (uint64_t)frame * stream.rate / stream.fps;
}

static uint32_t live_refill_frames(void)
{
    // Leave one decoded group free so playback can resume at any frame rate.
    uint32_t target = stream.fps * 3u;
    uint32_t capacity = (stream.groups - 1u) * stream.group_frames;
    return target < capacity ? target : capacity;
}

static void service_live_audio(void)
{
    if (!stream.started || stream.cancelled)
        return;
    uint32_t played = (uint32_t)(en_audio_elapsed() * stream.fps);
    en_audio_tick_with_refill(stream.loaded == stream.count ||
                              (played < stream.loaded && stream.loaded - played >= live_refill_frames()));
}

static void read_ima_frame(const uint8_t *block, uint32_t offset)
{
    if (offset < stream.ima_sample)
        return;
    for (unsigned ch = 0; ch < stream.channels; ch++) {
        if (!offset) {
            const uint8_t *header = block + 4u * ch;
            stream.ima_predictor[ch] = hb_ima_predictor(header);
            stream.ima_index[ch] = header[2];
        } else {
            uint32_t code = offset - 1u;
            const uint8_t *group = block + (1u + code / 8u) * 4u * stream.channels;
            uint8_t packed = group[4u * ch + (code % 8u) / 2u];
            unsigned nibble = (packed >> ((code & 1u) * 4u)) & 15u;
            hb_ima_sample(nibble, &stream.ima_predictor[ch], &stream.ima_index[ch]);
        }
    }
    stream.ima_sample = offset + 1u;
}

static uint32_t pull_stream_audio(int16_t *pcm, uint32_t frames, uint32_t advance, void *context)
{
    (void)advance;
    (void)context;
    uint64_t end = live_audio_boundary(stream.loaded) * stream.repeat;
    if (end <= stream.audio_frame)
        return stream.loaded == stream.count ? 0 : EN_AUDIO_PULL_WAIT;
    if (frames > end - stream.audio_frame)
        frames = end - stream.audio_frame;

    uint64_t sample = stream.audio_frame / stream.repeat;
    unsigned phase = stream.audio_frame % stream.repeat;
    uint32_t left_to_write = frames;
    while (left_to_write) {
        if (sample >= stream.audio_group_end) {
            stream.audio_group = sample ? stream.audio_group + 1 : 0;
            stream.audio_group_start = stream.audio_group_end;
            stream.audio_group_end = live_audio_boundary(((uint64_t)stream.audio_group + 1u) *
                                                          stream.group_frames);
            stream.ima_sample = 0;
        }
        uint32_t offset = sample - stream.audio_group_start;
        const uint8_t *block = stream.audio + stream.audio_group % stream.groups * stream.audio_slot_size;
        int16_t left, right;
        if (stream.adpcm) {
            read_ima_frame(block, offset);
            left = (int16_t)stream.ima_predictor[0];
            right = stream.channels == 2 ? (int16_t)stream.ima_predictor[1] : left;
        } else {
            const uint8_t *bytes = block + offset * stream.channels;
            left = hb_mulaw_decode(bytes[0]);
            right = stream.channels == 2 ? hb_mulaw_decode(bytes[1]) : left;
        }
        unsigned copies = stream.repeat - phase;
        if (copies > left_to_write)
            copies = left_to_write;
        left_to_write -= copies;
        for (unsigned i = 0; i < copies; i++) {
            *pcm++ = left;
            *pcm++ = right;
        }
        sample++;
        phase = 0;
    }
    stream.audio_frame += frames;
    return frames;
}

bool hb_h264_live_room(void)
{
    if (!stream_open() || stream.closing || stream.pending_pool || stream.loaded >= stream.count)
        return false;
    unsigned group = stream.loaded / stream.group_frames;
    if (group < stream.groups)
        return true;
    if (stream.shown == UINT32_MAX || stream.shown / stream.group_frames <= group - stream.groups)
        return false;
#ifdef HB_VIDEO_LAYER
    if (stream.direct_display) {
        uint64_t boundary = live_audio_boundary((uint64_t)(group - stream.groups + 1u) * stream.group_frames);
        if (!hb_video_layer_idle() || stream.audio_frame < boundary * stream.repeat)
            return false;
    }
#endif
    return true;
}

static const char *read_live_group(h264_clip *clip, bool (*read)(void *, uint32_t, void *),
                                   void *context, uint32_t length)
{
    if (!read || !hb_h264_live_room())
        return "H.264 player has no room for another group.";
    unsigned count = stream.count - stream.loaded;
    if (count > stream.group_frames)
        count = stream.group_frames;
    unsigned slot = stream.loaded / stream.group_frames % stream.groups;
    *clip = (h264_clip){.config = {stream.config, stream.config_size}, .count = count,
                        .cropped_output = stream.pixels[slot],
                        .width = stream.width, .height = stream.height,
                        .native = stream.native, .service_audio = service_live_audio};
    for (unsigned i = 0; i < count; i++) {
        uint8_t word[4];
        if (length < sizeof(word) || !read(word, sizeof(word), context))
            return "Incomplete H.264 frame header.";
        length -= sizeof(word);
        uint32_t size = little_word(word);
        if (!size || size > length || size > VIDEO_H264_MAX_SAMPLE ||
            !read(stream.samples[i], size, context) || !valid_sample(stream.samples[i], size, i))
            return "Invalid H.264 frame or missing keyframe.";
        clip->samples[i] = (h264_sample){stream.samples[i], size};
        length -= size;
    }
    uint32_t frames = live_audio_boundary((uint64_t)stream.loaded + count) -
                      live_audio_boundary(stream.loaded);
    uint32_t audio_bytes = group_audio_bytes(frames, stream.channels, stream.adpcm);
    uint8_t *block = stream.audio + slot * stream.audio_slot_size;
    if (length != audio_bytes || audio_bytes > stream.audio_slot_size ||
        !read(block, audio_bytes, context))
        return "H.264 group audio length mismatch.";
    if (stream.adpcm)
        for (unsigned ch = 0; ch < stream.channels; ch++)
            if (block[4u * ch + 2u] > 88u)
                return "Invalid H.264 group ADPCM index.";
    return NULL;
}

static void record_group_decode(uint32_t elapsed)
{
    hb_trace_log("VDecMs", stream.loaded, elapsed / 1000u);
#ifdef NANO_YOUTUBE_VIDEO
    measure_duration(&video_perf.group, elapsed);
#endif
}

static const char *prepare_live_frame(const h264_clip *clip, const decoder_frame *frame, unsigned index)
{
#ifdef HB_VIDEO_LAYER
    if (stream.direct_display) {
#ifdef NANO_YOUTUBE_VIDEO
        uint32_t started = hb_time_uptime_us();
#endif
        copy_nv12_frame(frame, index);
#ifdef NANO_YOUTUBE_VIDEO
        measure_time(&video_perf.convert, started);
#endif
        return NULL;
    }
#endif
    return convert_decoded_frame(clip, frame, index);
}

const char *hb_h264_live_group(bool (*read)(void *, uint32_t, void *), void *context, uint32_t length)
{
    h264_clip clip;
    const char *error = read_live_group(&clip, read, context, length);
    if (error)
        return error;
    uint32_t started = hb_time_uptime_us();
#ifdef HB_VIDEO_LAYER
    if (stream.direct_display) {
        void *pool;
        decoder_frame frames[VIDEO_H264_MAX_FRAMES];
        error = decode_to_planes(&clip, frames, &pool);
        if (!error) {
            for (unsigned i = 0; i < clip.count; i++)
                prepare_live_frame(&clip, &frames[i], i);
            error = release_decode_pool(pool, NULL);
        }
    } else
#endif
        error = decode_clip(&clip);
    record_group_decode(hb_time_uptime_us() - started);
    if (error) {
        disk_checkpoints = true;
        log_used = 0;
        checkpoint("LiveErr", stream.loaded, clip.count);
        hb_fs_write("/Data/VideoStream-h264-live-error.txt", error, strlen(error));
        disk_checkpoints = false;
        return error;
    }
    stream.loaded += clip.count;
    return NULL;
}

const char *hb_h264_live_queue(bool (*read)(void *, uint32_t, void *), void *context, uint32_t length)
{
    const char *error = read_live_group(&stream.pending_clip, read, context, length);
    if (error)
        return error;
    uint32_t started = hb_time_uptime_us();
    error = decode_to_planes(&stream.pending_clip, stream.pending_frames, &stream.pending_pool);
    if (error == decoder_low_memory)
        return startup_failure(error, stream.config, stream.config_size, stream.fps, stream.rate, stream.channels);
    if (error)
        return error;
    stream.pending_decode_us = hb_time_uptime_us() - started;
    stream.converted = 0;
    return NULL;
}

bool hb_h264_live_group_pending(void) { return stream.pending_pool != NULL; }

const char *hb_h264_live_advance(void)
{
    if (!stream.pending_pool)
        return NULL;
    /* The pool owns the decoded planes until display preparation finishes. */
    uint32_t started = hb_time_uptime_us();
    const char *error = prepare_live_frame(&stream.pending_clip, &stream.pending_frames[stream.converted],
                                          stream.converted);
    stream.pending_decode_us += hb_time_uptime_us() - started;
    if (error || ++stream.converted == stream.pending_clip.count) {
        error = release_decode_pool(stream.pending_pool, error);
        stream.pending_pool = NULL;
        record_group_decode(stream.pending_decode_us);
        if (!error)
            stream.loaded += stream.pending_clip.count;
    }
    return error;
}

static bool read_group_memory(void *out, uint32_t size, void *context)
{
    const uint8_t **cursor = context;
    memcpy(out, *cursor, size);
    *cursor += size;
    return true;
}

const char *hb_h264_live_submit(const void *data, uint32_t length)
{
    if (!data || !length)
        return "Provide a complete H.264 group.";
    const uint8_t *cursor = data;
    return hb_h264_live_group(read_group_memory, &cursor, length);
}

const char *hb_h264_live_error(void) { return live_playback_error; }

int hb_h264_live_tick(void)
{
    live_playback_error = NULL;
    if (!stream_open() || stream.cancelled)
        return 1;
    if (!stream.started) {
        uint32_t opening_frames = stream.native ? stream.groups * stream.group_frames : live_refill_frames();
        if (stream.loaded < opening_frames && stream.loaded < stream.count)
            return 0;
        stream.canvas = open_movie_canvas(stream.direct_display);
        if (!stream.canvas) {
            live_playback_error = "Could not open the video display.";
            return -1;
        }
        lv_obj_add_event_cb(stream.canvas, live_canvas_deleted, LV_EVENT_DELETE, NULL);
        stream.started = true;
        uint32_t output_rate = stream.rate * stream.repeat;
        bool audio_started = stream.native ? en_audio_start_video_stream(output_rate, pull_stream_audio, NULL) :
                                             en_audio_start_live_stream(output_rate, pull_stream_audio, NULL);
        if (!audio_started) {
            live_playback_error = "Could not start video audio.";
            return -1;
        }
    }
    service_live_audio();
    if (en_audio_state() == EN_AUDIO_FAILED) {
        live_playback_error = "Video audio stopped.";
        return -1;
    }
    if (en_audio_state() == EN_AUDIO_IDLE)
        return 1;
    double elapsed = en_audio_elapsed();
    unsigned frame = elapsed * stream.fps;
#ifdef NANO_YOUTUBE_VIDEO
    if (!video_paused) {
        bool buffering = en_audio_stream_buffering();
        if (buffering && !video_perf.buffering)
            video_perf.stalls++;
        video_perf.buffering = buffering;
    }
#endif
#ifdef NANO_VIDEO_STREAM
    update_video_controls(elapsed);
#endif
    if (frame >= stream.loaded)
        frame = stream.loaded - 1u;
    bool controls_visible = false;
#ifdef HB_VIDEO_LAYER
#ifdef NANO_VIDEO_STREAM
    controls_visible = stream.direct_display && video_controls &&
                       !lv_obj_has_flag(video_controls, LV_OBJ_FLAG_HIDDEN);
    if (controls_visible) {
        uint32_t now = hb_time_uptime_ms();
        if ((stream.refresh_pending || now - stream.ui_refresh_at >= 100u) && hb_video_layer_show_ui()) {
            stream.ui_refresh_at = now;
            stream.refresh_pending = false;
        }
        stream.refresh_blocked_at = 0;
    }
#endif
#endif
    if (frame != stream.shown || stream.refresh_pending) {
#ifdef NANO_YOUTUBE_VIDEO
        uint32_t started = hb_time_uptime_us();
#endif
        unsigned slot = frame / stream.group_frames % stream.groups;
        unsigned index = frame % stream.group_frames;
#ifdef HB_VIDEO_LAYER
        if (stream.direct_display) {
            if (!controls_visible) {
                if (!present_nv12_frame(slot, index)) {
#ifdef NANO_YOUTUBE_VIDEO
                    video_perf.refresh_retries++;
#endif
                    uint32_t now = hb_time_uptime_ms();
                    if (!stream.refresh_blocked_at)
                        stream.refresh_blocked_at = now;
                    else if (now - stream.refresh_blocked_at >= 1000u) {
                        live_playback_error = "The hardware video layer stopped refreshing.";
                        return -1;
                    }
                    return 0;
                }
                stream.refresh_blocked_at = 0;
                stream.refresh_pending = false;
            }
        } else
#endif
            video_landscape_present_cropped(stream.canvas,
                stream.pixels[slot] + index * stream.width * stream.height, stream.width, stream.height);
        if (stream.shown != UINT32_MAX && frame > stream.shown + 1u) {
            hb_trace_log("VSkip", stream.shown, frame);
#ifdef NANO_YOUTUBE_VIDEO
            video_perf.skipped += frame - stream.shown - 1u;
#endif
        }
        stream.shown = frame;
#ifdef NANO_YOUTUBE_VIDEO
        if (!controls_visible) {
            measure_time(&video_perf.draw, started);
            video_perf.presented++;
        }
#else
        (void)controls_visible;
#endif
    }
    return 0;
}

uint32_t hb_h264_live_buffer_ms(void)
{
    if (!stream.fps)
        return 0;
    double ms = 1000.0 * (stream.loaded / (double)stream.fps -
                         (stream.started ? en_audio_elapsed() : 0.0));
    return ms > 0 ? (uint32_t)ms : 0;
}

bool hb_h264_live_playing(void) { return stream.started; }
#ifdef NANO_VIDEO_STREAM
bool hb_h264_live_paused(void)
{
    return stream.started && video_paused;
}
#endif
bool hb_h264_live_cancelled(void) { return stream.cancelled; }
bool hb_h264_live_complete(void) { return stream_open() && stream.loaded == stream.count; }

bool hb_h264_live_set_final_count(uint32_t count)
{
    if (stream.count != UINT32_MAX || count <= stream.loaded ||
        count - stream.loaded > stream.group_frames)
        return false;
    stream.count = count;
    return true;
}

bool hb_h264_live_finish(void)
{
    if (!stream.loaded || stream.count != UINT32_MAX)
        return false;
    stream.count = stream.loaded;
    return true;
}

static uint32_t max_disk_group_bytes(void)
{
    return stream.group_frames * (4u + VIDEO_H264_MAX_SAMPLE) + stream.audio_slot_size;
}

static void finish_spooled_stream(void)
{
    if (!stream.end_reason)
        stream.end_reason = stream.cancelled ? "Stopped by the user; buffers released" :
                            "Playback finished; buffers released";
    hb_h264_live_stop();
}

static void advance_spooled_stream(lv_timer_t *timer)
{
    (void)timer;
    if (stream.closing) {
        if (stream.from_disk)
            finish_spooled_stream();
        else
            hb_h264_live_stop();
        return;
    }
    const char *error = "Invalid H.264 group in the saved video.";
    bool serviced = stream.started && !stream.cancelled;
    if (serviced && !stream.audio_eof_logged && stream.loaded == stream.count &&
        stream.audio_frame == live_audio_boundary(stream.loaded) * stream.repeat) {
        stream.audio_eof_logged = true;
        log_saved_video_end("Final audio samples handed to the mixer");
    }
    /* Service audio before a potentially long flash read and hardware
       decode so the mixer can play queued samples during that work. */
    if (serviced) {
        int result = hb_h264_live_tick();
        if (result > 0) {
            finish_spooled_stream();
            return;
        }
        if (result < 0) {
            error = hb_h264_live_error();
            goto invalid;
        }
    }
    if (!stream.cancelled && stream.pending_pool) {
        if (stream.pending_clip.count == stream.count - stream.loaded && !stream.converted)
            log_saved_video_end("Converting the final video group");
        error = hb_h264_live_advance();
        if (error)
            goto invalid;
        if (stream.loaded == stream.count)
            log_saved_video_end("Final video frames decoded");
    } else if (!stream.cancelled && hb_h264_live_room()) {
        bool last_group = stream.count - stream.loaded <= stream.group_frames;
        if (last_group)
            log_saved_video_end("Reading the final video group from flash");
        uint8_t header[8];
        if (!read_media(header, sizeof(header)))
            goto invalid;
        uint32_t length = little_word(header);
        uint32_t count = little_word(header + 4);
        if (!length || !count || count > stream.group_frames ||
            count != (stream.count - stream.loaded < stream.group_frames ?
                      stream.count - stream.loaded : stream.group_frames))
            goto invalid;
        if (length > max_disk_group_bytes())
            goto invalid;
        uint32_t started = hb_time_uptime_us();
        // The SDK file reader has a per-call limit. Read the group in bounded chunks.
        for (uint32_t offset = 0; offset < length;) {
            uint32_t size = length - offset;
            if (size > 128u * 1024u)
                size = 128u * 1024u;
            if (!read_media(stream.disk_group + offset, size))
                goto invalid;
            offset += size;
        }
        hb_trace_log("VReadMs", stream.loaded, (hb_time_uptime_us() - started) / 1000u);
#ifdef NANO_YOUTUBE_VIDEO
        measure_time(&video_perf.read, started);
#endif
        const uint8_t *data = stream.disk_group;
        if (last_group)
            log_saved_video_end("Submitting the final video group to the hardware decoder");
        error = hb_h264_live_queue(read_group_memory, &data, length);
        if (error)
            goto invalid;
    } else if (serviced) {
        return;
    }
    int result = hb_h264_live_tick();
    if (result > 0) {
        finish_spooled_stream();
    } else if (result < 0) {
        error = hb_h264_live_error();
        goto invalid;
    }
    return;
invalid:
    stream.end_reason = error;
    hb_h264_live_stop();
    movie_failed(error);
}

const char *hb_h264_play_groups(uint32_t count, uint32_t fps, uint32_t rate,
                                  uint32_t channels, const uint8_t *config, uint32_t size,
                                  unsigned group_frames)
{
    if (!hb_fs_media_reader_open(VIDEO_H264_RECEIVE_PATH)) {
        hb_fs_remove(VIDEO_H264_RECEIVE_PATH);
        return "Could not open the received video.";
    }
    if (!begin_group_playback(config, size, count, fps, rate, channels, group_frames, true)) {
        hb_fs_media_reader_close();
        hb_fs_remove(VIDEO_H264_RECEIVE_PATH);
        return hb_h264_live_error();
    }
    stream.from_disk = true;
    log_saved_video_end("Saved video opened");
    hb_trace_log("VDisk", count, fps);
    stream.timer = lv_timer_create(advance_spooled_stream, 8, NULL);
    if (!stream.timer) {
        hb_h264_live_stop();
        return "Could not start the video player.";
    }
    return NULL;
}

static bool stream_open(void)
{
#ifdef HB_VIDEO_LAYER
    if (stream.direct_display)
        return stream.nv12_allocations[0] != NULL;
#endif
    return stream.pixels[0] != NULL;
}

static int (*audio_unload)(void);

bool hb_h264_init(void)
{
    if (!hb_shared)
        return false;
    if (!audio_unload) {
        int (*previous_unload)(void) = hb_shared->prepare_unload;
        if (!en_audio_init())
            return false;
        audio_unload = hb_shared->prepare_unload;
        hb_shared->prepare_unload = previous_unload ? previous_unload : hb_h264_prepare_unload;
    }
    return true;
}

int hb_h264_prepare_unload(void)
{
    if (!hb_h264_stop())
        return 0;
    return audio_unload ? audio_unload() : 1;
}
