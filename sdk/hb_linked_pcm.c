#include "hb_linked_pcm.h"
#include "hb_heap.h"
#include "hb_sdk.h"
#include "lvgl/lvgl.h"

#include <string.h>

#define BLOCK_MS 400u
#define VIDEO_DESCRIPTORS 8u
#define PCM_SLOTS 3u
#define HEAP_MARGIN (128u * 1024u)
#define SETTLE_MS 100u
#define DESCRIPTOR_BYTES 0x80u
#define DESCRIPTOR_LENGTH 0x0cu
#define DESCRIPTOR_VOICE 0x48u
#define DESCRIPTOR_NEXT 0x54u
#define DESCRIPTOR_PLAYING 0x64u
#define VOICE_DESCRIPTOR 0x78u
#define VOICE_CALLBACK 0xa4u
#define VOICE_READER 0xa8u
#define PCM_READER_VTABLE 0x087b6dd4u

typedef uint32_t descriptor_t[DESCRIPTOR_BYTES / 4u];
typedef void (*completion_fn)(void *, void *, unsigned);
typedef void (*play_fn)(void *, void *, completion_fn, void *);
enum stage_result { STAGE_READY, STAGE_WAIT, STAGE_END, STAGE_FAILED };

static struct {
    lv_timer_t *timer;
    descriptor_t *blocks;
    int16_t *pcm;
    int16_t *staging;
    en_audio_pull_fn pull;
    void *context;
    void *player;
    uintptr_t voice;
    uint64_t elapsed_frames;
    uint32_t rate, block_frames, total_frames;
    uint32_t count, descriptor_count, prepared, active, run_first, staged_frames;
    uint32_t started_ms, settled_ms, completion, refills;
    bool running, settling, paused, stopping, failed;
    bool live, rolling, input_waiting, refill_ready, rebuffering;
    en_audio_state_t state;
} stream;

static uint32_t read_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}

static bool ram_object(uintptr_t address, unsigned length)
{
    return !(address & 3u) && address >= 0x08000000u && address <= 0x0c000000u - length;
}

static uint32_t block_frames(uint32_t index)
{
    uint32_t left = stream.total_frames - index * stream.block_frames;
    return left < stream.block_frames ? left : stream.block_frames;
}

static uint32_t *descriptor(uint32_t index)
{
    if (stream.rolling)
        /* RetailOS retains this pointer as the completion callback argument. */
        return stream.blocks[index == stream.run_first ? VIDEO_DESCRIPTORS : index % VIDEO_DESCRIPTORS];
    return stream.blocks[index % stream.descriptor_count];
}

static uint32_t submitted_frames(uint32_t index)
{
    return descriptor(index)[DESCRIPTOR_LENGTH / 4u] / 4u;
}

static int16_t *pcm_slot(uint32_t index)
{
    return stream.pcm + (index % PCM_SLOTS) * stream.block_frames * 2u;
}

static bool descriptor_playing(uint32_t index)
{
    return ((const volatile uint8_t *)descriptor(index))[DESCRIPTOR_PLAYING] != 0;
}

static bool reader_position(uint32_t index, uint32_t *position)
{
    uintptr_t desc = (uintptr_t)descriptor(index);
    uintptr_t voice = read_word(desc + DESCRIPTOR_VOICE);
    if (!ram_object(voice, VOICE_READER + 4u) || read_word(voice + VOICE_DESCRIPTOR) != desc)
        return false;
    uintptr_t reader = read_word(voice + VOICE_READER);
    uintptr_t base = (uintptr_t)pcm_slot(index);
    uintptr_t end = base + submitted_frames(index) * 4u;
    if (!ram_object(reader, 0x3cu) || read_word(reader) != PCM_READER_VTABLE ||
        read_word(reader + 0x34u) != base || read_word(reader + 0x38u) != end)
        return false;
    uintptr_t cursor = read_word(reader + 0x30u);
    if ((cursor & 3u) || cursor < base || cursor > end ||
        read_word(voice + VOICE_READER) != reader || read_word(voice + VOICE_DESCRIPTOR) != desc ||
        read_word(desc + DESCRIPTOR_VOICE) != voice)
        return false;
    *position = (cursor - base) / 4u;
    stream.voice = voice;
    return true;
}

static void mixer_completed(void *first, void *context, unsigned status)
{
    (void)first;
    (void)context;
    __atomic_store_n(&stream.completion, status, __ATOMIC_RELEASE);
}

static void release_playback(void)
{
    for (uint32_t i = 0; i < stream.descriptor_count; i++) {
        descriptor_t *block = &stream.blocks[i];
        /* The stock destructor owns +8 for file-loaded audio. Our PCM is
           shared across descriptors and is freed once, below. */
        (*block)[0x08u / 4u] = 0;
        ((void (*)(void *))0x0807565du)(block);
    }
    hb_os_free(stream.blocks);
    hb_os_free(stream.pcm);
    stream.blocks = NULL;
    stream.pcm = NULL;
    stream.count = 0;
    lv_timer_delete(stream.timer);
    stream.timer = NULL;
    stream.state = stream.failed ? EN_AUDIO_FAILED : EN_AUDIO_IDLE;
    hb_trace_log(stream.failed ? "GapFail" : "GapEnd", (uint32_t)stream.elapsed_frames, stream.refills);
}

static enum stage_result stage_pcm(void)
{
    uint32_t frames = stream.rolling ? stream.block_frames : block_frames(stream.prepared);
    if (stream.rolling && stream.count != UINT32_MAX)
        return stream.staged_frames ? STAGE_READY : STAGE_END;
    while (stream.staged_frames < frames) {
        uint32_t want = frames - stream.staged_frames;
        uint32_t got = stream.pull(stream.staging + stream.staged_frames * 2u,
                                   want, want, stream.context);
        if (got == EN_AUDIO_PULL_WAIT && stream.live) {
            stream.input_waiting = true;
            return STAGE_WAIT;
        }
        if (!got && stream.rolling) {
            stream.count = stream.prepared + (stream.staged_frames != 0);
            stream.input_waiting = false;
            return stream.staged_frames ? STAGE_READY : STAGE_END;
        }
        if (!got || got > want)
            return STAGE_FAILED;
        stream.staged_frames += got;
    }
    stream.input_waiting = false;
    return STAGE_READY;
}

static void copy_staged_pcm(void)
{
    memcpy(pcm_slot(stream.prepared), stream.staging, stream.staged_frames * 4u);
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool queue_staged_pcm(void)
{
    uint32_t index = stream.prepared;
    uint32_t *next = descriptor(index);
    if (descriptor_playing(index) || read_word((uintptr_t)next + DESCRIPTOR_VOICE))
        return false;
    next[0x04u / 4u] = next[0x08u / 4u] = (uintptr_t)pcm_slot(index);
    next[DESCRIPTOR_LENGTH / 4u] = stream.staged_frames * 4u;
    next[DESCRIPTOR_NEXT / 4u] = 0;
    if (index != stream.run_first)
        __atomic_store_n(&descriptor(index - 1u)[DESCRIPTOR_NEXT / 4u], (uintptr_t)next, __ATOMIC_RELEASE);
    stream.staged_frames = 0;
    stream.prepared++;
    return true;
}

static void start_run(void)
{
    if (stream.rebuffering && !stream.refill_ready)
        return;
    stream.rebuffering = false;
    while (stream.prepared < stream.count && stream.prepared - stream.run_first < PCM_SLOTS) {
        enum stage_result result = stage_pcm();
        if (result == STAGE_END)
            break;
        if (result == STAGE_WAIT) {
            /* With only two decoded video groups, the last displayed group
               can pin one slot. Two blocks fit in a single incoming group. */
            if (stream.rolling && stream.prepared - stream.run_first >= 2u)
                break;
            return;
        }
        if (result == STAGE_FAILED) {
            stream.failed = true;
            return;
        }
        copy_staged_pcm();
        if (!queue_staged_pcm()) {
            stream.failed = true;
            return;
        }
    }
    if (stream.run_first == stream.prepared)
        return;
    stream.active = stream.run_first;
    stream.settling = false;
    stream.running = true;
    stream.started_ms = hb_time_uptime_ms();
    __atomic_store_n(&stream.completion, 0, __ATOMIC_RELEASE);
    uint32_t *first = descriptor(stream.run_first);
    ((play_fn)0x0841828du)(stream.player, first, mixer_completed, NULL);
    uintptr_t voice = read_word((uintptr_t)first + DESCRIPTOR_VOICE);
    stream.voice = ram_object(voice, VOICE_READER + 4u) ? voice : 0;
}

static bool any_native_owner(void)
{
    uint32_t first = stream.rolling ? 0 : stream.run_first;
    uint32_t end = stream.rolling ? stream.descriptor_count : stream.prepared;
    for (uint32_t i = first; i < end; i++) {
        uintptr_t desc = (uintptr_t)stream.blocks[i];
        if (*(const volatile uint8_t *)(desc + DESCRIPTOR_PLAYING) || read_word(desc + DESCRIPTOR_VOICE))
            return true;
    }
    return false;
}

static void finish_run(uint32_t now, unsigned completion)
{
    if (any_native_owner() || (stream.voice &&
        read_word(stream.voice + VOICE_CALLBACK) == (uintptr_t)mixer_completed)) {
        stream.settling = false;
        return;
    }
    if (!stream.settling) {
        stream.settling = true;
        stream.settled_ms = now;
        return;
    }
    if (now - stream.settled_ms < SETTLE_MS)
        return;
    stream.running = false;
    if (completion != 1u) {
        stream.failed = true;
        return;
    }
    uint32_t tail = stream.prepared - 1u;
    stream.elapsed_frames = (uint64_t)tail * stream.block_frames + submitted_frames(tail);
    stream.run_first = stream.prepared;
    if (!stream.paused && !stream.stopping && stream.prepared < stream.count) {
        stream.rebuffering = stream.live && stream.input_waiting;
        hb_trace_log("GapDrain", (uint32_t)stream.elapsed_frames, stream.rebuffering);
    }
}

static void poll_run(void)
{
    uint32_t now = hb_time_uptime_ms();
    unsigned completion = __atomic_load_n(&stream.completion, __ATOMIC_ACQUIRE);
    if (completion) {
        finish_run(now, completion);
        return;
    }
    uint32_t found = stream.prepared;
    for (uint32_t i = stream.active; i < stream.prepared; i++) {
        if (descriptor_playing(i)) {
            found = i;
            break;
        }
    }
    if (found == stream.prepared) {
        /* A normal handoff can briefly have both flags clear. Keep a quiet
           interval before treating an unexpected removal as failure. */
        if (stream.voice || now - stream.started_ms >= 1000u)
            finish_run(now, __atomic_load_n(&stream.completion, __ATOMIC_ACQUIRE));
        return;
    }

    stream.settling = false;
    stream.active = found;
    uint32_t position;
    if (!reader_position(found, &position))
        return;
    uint64_t elapsed = (uint64_t)found * stream.block_frames + position;
    if (elapsed > stream.elapsed_frames)
        stream.elapsed_frames = elapsed;
    if (stream.stopping || stream.paused || stream.failed || stream.prepared == stream.count)
        return;
    if (stream.prepared < found + 2u)
        return;
    /* Decode into staging as soon as the previous refill is published. The
       mixer may still be reading the slot that the staged block will use. */
    enum stage_result result = stage_pcm();
    if (result != STAGE_READY) {
        stream.failed = result == STAGE_FAILED;
        return;
    }
    if (stream.prepared != found + 2u || !reader_position(found, &position))
        return;
    /* The old contents of the slot have finished playing, and one full
       block still separates us from its next use. */
    uint32_t lead = stream.block_frames / 4u;
    if (submitted_frames(found) - position < lead)
        return;
    copy_staged_pcm();
    /* Publish only filled PCM. Keep the staged block if we miss the window
       so it can start the next run after the native voice drains. */
    if (__atomic_load_n(&stream.completion, __ATOMIC_ACQUIRE) ||
        !reader_position(found, &position) || submitted_frames(found) - position < lead)
        return;
    if (!queue_staged_pcm() || __atomic_load_n(&stream.completion, __ATOMIC_ACQUIRE))
        stream.failed = true;
    stream.refills++;
}

static void poll_playback(lv_timer_t *timer)
{
    (void)timer;
    if (stream.running)
        poll_run();
    if (stream.running)
        return;
    if (stream.stopping || stream.failed || stream.run_first == stream.count) {
        release_playback();
        return;
    }
    if (!stream.paused)
        start_run();
    if (stream.failed || (!stream.running && stream.run_first == stream.count))
        release_playback();
}

static int start_linked_pcm(uint32_t rate, uint64_t frames, en_audio_pull_fn pull,
                           void *context, uint32_t volume, bool live, bool rolling, unsigned block_ms)
{
    if (stream.blocks || !pull || (!rolling && !frames) || frames > UINT32_MAX ||
        (rate != 22050u && rate != 44100u) ||
        hb_media_state() == 0 ||
        read_word(0x0841828cu) != 0x5fffe92du || read_word(0x08630094u) != 0xe58560a8u ||
        read_word(0x08631584u) != 0xe5901054u || read_word(0x08664c34u) != 0xe5801030u ||
        read_word(0x0807565cu) != 0x4604b510u || read_word(0x086315e4u) != 0xe12fff33u)
        return 0;

    uint32_t frames_per_block = rate * block_ms / 1000u;
    uint32_t count = rolling ? UINT32_MAX : (frames + frames_per_block - 1u) / frames_per_block;
    uint32_t descriptor_count = rolling ? VIDEO_DESCRIPTORS + 1u : count;
    uint32_t pcm_bytes = (PCM_SLOTS + 1u) * frames_per_block * 4u;
    /* Budget for the stock descriptor's small mutex as well as the array. */
    uint32_t required = pcm_bytes + descriptor_count * (DESCRIPTOR_BYTES + 32u);
    if (hb_os_heap_largest() < required + HEAP_MARGIN)
        return 0;
    void *player = ((void *(*)(void))0x08417eb9u)();
    if (!player)
        return 0;
    descriptor_t *blocks = hb_os_alloc(descriptor_count * DESCRIPTOR_BYTES);
    int16_t *pcm = hb_os_alloc(pcm_bytes);
    if (!blocks || !pcm) {
        hb_os_free(blocks);
        hb_os_free(pcm);
        return 0;
    }
    lv_timer_t *timer = lv_timer_create(poll_playback, 8u, NULL);
    if (!timer) {
        hb_os_free(blocks);
        hb_os_free(pcm);
        return 0;
    }

    memset(&stream, 0, sizeof(stream));
    stream.timer = timer;
    stream.blocks = blocks;
    stream.pcm = pcm;
    stream.staging = pcm + PCM_SLOTS * frames_per_block * 2u;
    stream.pull = pull;
    stream.context = context;
    stream.player = player;
    stream.rate = rate;
    stream.block_frames = frames_per_block;
    stream.total_frames = (uint32_t)frames;
    stream.count = count;
    stream.descriptor_count = descriptor_count;
    stream.live = live;
    stream.rolling = rolling;
    stream.state = EN_AUDIO_PLAYING;
    for (uint32_t i = 0; i < descriptor_count; i++) {
        uint32_t *desc = blocks[i];
        hb_audio_ctor(desc);
        ((uint8_t *)desc)[0x10u] = 0;
        desc[0x14u / 4u] = rate;
        desc[0x18u / 4u] = 2;
        desc[0x1cu / 4u] = 16;
        hb_audio_setfields(desc, volume);
    }
    hb_trace_log("GapStart", descriptor_count, rate);
    start_run();
    if (stream.failed) {
        release_playback();
        return -1;
    }
    return 1;
}

int hb_linked_pcm_start(uint32_t rate, uint64_t frames, en_audio_pull_fn pull,
                        void *context, uint32_t volume, bool live)
{
    if (!hb_fs_exists(live ? "/Data/YouTube-gapless-live" : "/Data/YouTube-gapless-saved"))
        return 0;
    return start_linked_pcm(rate, frames, pull, context, volume, live, false, BLOCK_MS);
}

int hb_linked_pcm_start_video(uint32_t rate, en_audio_pull_fn pull, void *context,
                              uint32_t volume, unsigned block_ms)
{
    if (block_ms != 60u && block_ms != 120u)
        return 0;
    return start_linked_pcm(rate, 0, pull, context, volume, true, true, block_ms);
}

void hb_linked_pcm_tick(void)
{
    if (stream.blocks)
        poll_playback(stream.timer);
}

bool hb_linked_pcm_busy(void) { return stream.blocks != NULL; }

bool hb_linked_pcm_stop(void)
{
    if (stream.blocks) {
        stream.stopping = true;
        /* Also advance teardown when Home has suspended the app's timer. */
        poll_playback(stream.timer);
    }
    return !stream.blocks;
}

void hb_linked_pcm_pause(bool paused)
{
    if (!stream.blocks || stream.stopping || stream.failed)
        return;
    stream.paused = paused;
    stream.state = paused ? EN_AUDIO_PAUSED : EN_AUDIO_PLAYING;
}

void hb_linked_pcm_set_refill_ready(bool ready) { stream.refill_ready = ready; }

bool hb_linked_pcm_buffering(void)
{
    return stream.live && stream.blocks && !stream.running && !stream.paused && !stream.stopping;
}

en_audio_state_t hb_linked_pcm_state(void) { return stream.state; }

double hb_linked_pcm_elapsed(void) { return (double)stream.elapsed_frames / stream.rate; }

void hb_linked_pcm_queue_status(unsigned *queued, unsigned *playing_mask)
{
    *queued = stream.running && stream.prepared > stream.active ? stream.prepared - stream.active : 0;
    *playing_mask = stream.running && descriptor_playing(stream.active) ? 1u << (stream.active % PCM_SLOTS) : 0;
}
