#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_shared.h"
#include "lvgl/lvgl.h"

#define RATE 22050u
#define BLOCK_MS 400u
#define BLOCK_FRAMES (RATE * BLOCK_MS / 1000u)
#define BLOCK_BYTES (BLOCK_FRAMES * 4u)
#define PCM_SLOTS 3u
#define BLOCKS 32u
#define PLAYING_OFFSET 0x64u
#define VOICE_OFFSET 0x48u
#define VOICE_DESCRIPTOR 0x78u
#define VOICE_READER 0xa8u
#define PCM_READER_VTABLE 0x087b6dd4u
#define QUIET_MS 100u
#define MIN_LEAD_BYTES (RATE / 10u * 4u)

/* Every descriptor is submitted once. Only the three PCM arrays are reused. */
static uint32_t descriptors[BLOCKS][0x80u / sizeof(uint32_t)];
static int16_t (*pcm)[BLOCK_FRAMES * 2u];
static lv_timer_t *poll_timer;
static lv_obj_t *status_label;
static int (*previous_unload)(void);
static unsigned next_fill, refilled, missed, observed, highest;
static uint32_t started_at, last_owned_at, worst_fill_us, mixer_rate;

static uint32_t read_word(uintptr_t address)
{
    return *(const volatile uint32_t *)address;
}

static bool ram_object(uintptr_t address, unsigned length)
{
    return !(address & 3u) && address >= 0x08000000u && address <= 0x0c000000u - length;
}

static void fill_tone(unsigned sequence)
{
    int16_t *samples = pcm[sequence % PCM_SLOTS];
    unsigned phase = sequence * BLOCK_FRAMES % 50u;
    for (unsigned i = 0; i < BLOCK_FRAMES; i++) {
        unsigned triangle = phase < 25u ? phase : 50u - phase;
        int16_t sample = (int)triangle * 320 - 4000;
        *samples++ = sample;
        *samples++ = sample;
        if (++phase == 50u)
            phase = 0;
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static bool has_refill_time(unsigned active, unsigned target)
{
    /* One full intervening block must still be ahead of the reader. */
    if (active + 2u != target)
        return false;
    uintptr_t desc = (uintptr_t)descriptors[active];
    uintptr_t voice = read_word(desc + VOICE_OFFSET);
    if (!ram_object(voice, VOICE_READER + 4u) || read_word(voice + VOICE_DESCRIPTOR) != desc)
        return false;
    uintptr_t reader = read_word(voice + VOICE_READER);
    uintptr_t base = (uintptr_t)pcm[active % PCM_SLOTS];
    /* The PCM reader advances +0x30 and resets it from the base at +0x34. */
    if (!ram_object(reader, 0x3cu) || read_word(reader) != PCM_READER_VTABLE ||
        read_word(reader + 0x34u) != base || read_word(reader + 0x38u) != base + BLOCK_BYTES)
        return false;
    uintptr_t cursor = read_word(reader + 0x30u);
    uint32_t rate = read_word(voice + 0x0cu);
    if ((cursor & 3u) || cursor < base || cursor > base + BLOCK_BYTES - MIN_LEAD_BYTES ||
        read_word(voice + VOICE_READER) != reader || read_word(voice + VOICE_DESCRIPTOR) != desc ||
        read_word(desc + VOICE_OFFSET) != voice)
        return false;
    mixer_rate = rate;
    return true;
}

static void report(const char *result)
{
    char message[256];
    int length = lv_snprintf(message, sizeof(message),
        "%s\nBlocks: %u/%u observed, highest %u\nPCM refills: %u/%u, missed %u\n"
        "Longest refill: %lu us\nPCM source: %u Hz, mixer: %lu Hz\n",
        result, observed, BLOCKS, highest + 1u, refilled, BLOCKS - PCM_SLOTS, missed,
        (unsigned long)worst_fill_us, RATE, (unsigned long)mixer_rate);
    if (length > 0 && (unsigned)length < sizeof(message))
        hb_fs_write("/Data/AudioProbe-refill.txt", message, (uint32_t)length);
    lv_label_set_text(status_label, message);
}

static void poll_playback(lv_timer_t *timer)
{
    uint32_t now = hb_time_uptime_ms();
    unsigned active = BLOCKS;
    bool owned = false;
    for (unsigned i = 0; i < BLOCKS; i++) {
        uintptr_t desc = (uintptr_t)descriptors[i];
        if (*(const volatile uint8_t *)(desc + PLAYING_OFFSET))
            active = i;
        owned |= read_word(desc + VOICE_OFFSET) != 0;
    }
    if (active != BLOCKS || owned)
        last_owned_at = now;
    if (active != BLOCKS) {
        if (!observed || active > highest) {
            highest = active;
            observed++;
        }
        while (next_fill < BLOCKS && next_fill < active + 2u) {
            missed++;
            next_fill++;
        }
        if (next_fill < BLOCKS && has_refill_time(active, next_fill)) {
            uint32_t start = hb_time_uptime_us();
            fill_tone(next_fill++);
            uint32_t elapsed = hb_time_uptime_us() - start;
            if (elapsed > worst_fill_us)
                worst_fill_us = elapsed;
            refilled++;
        }
        return;
    }
    bool reached_end = observed && highest == BLOCKS - 1u;
    if (owned || now - last_owned_at < QUIET_MS ||
        (!reached_end && now - started_at < BLOCKS * BLOCK_MS + 1500u))
        return;

    lv_timer_delete(timer);
    poll_timer = NULL;
    hb_os_free(pcm);
    pcm = NULL;
    hb_wake_lock(false);
    report(reached_end && refilled == BLOCKS - PCM_SLOTS ? "Probe finished." :
                                                           "Probe did not verify all refills.");
}

static void start_test(lv_event_t *event)
{
    (void)event;
    if (poll_timer)
        return;
    if (read_word(0x0841828cu) != 0x5fffe92du ||
        read_word(0x08630094u) != 0xe58560a8u || read_word(0x08631584u) != 0xe5901054u ||
        read_word(0x08664c34u) != 0xe5801030u) {
        lv_label_set_text(status_label, "This test needs the mapped stock audio firmware.");
        return;
    }
    if (hb_media_state() == 0) {
        lv_label_set_text(status_label, "Pause stock playback first.");
        return;
    }
    if (hb_os_heap_largest() >= PCM_SLOTS * BLOCK_BYTES + 128u * 1024u)
        pcm = hb_os_alloc(PCM_SLOTS * BLOCK_BYTES);
    if (!pcm) {
        lv_label_set_text(status_label, "Not enough memory for the test.");
        return;
    }
    poll_timer = lv_timer_create(poll_playback, 8, NULL);
    if (!poll_timer) {
        hb_os_free(pcm);
        pcm = NULL;
        lv_label_set_text(status_label, "Could not start the probe timer.");
        return;
    }
    for (unsigned i = 0; i < PCM_SLOTS; i++)
        fill_tone(i);
    for (unsigned i = 0; i < BLOCKS; i++) {
        uint32_t *desc = descriptors[i];
        hb_audio_ctor(desc);
        desc[0x04u / 4u] = desc[0x08u / 4u] = (uint32_t)(uintptr_t)pcm[i % PCM_SLOTS];
        desc[0x0cu / 4u] = BLOCK_BYTES;
        ((uint8_t *)desc)[0x10u] = 0;
        desc[0x14u / 4u] = RATE;
        desc[0x18u / 4u] = 2;
        desc[0x1cu / 4u] = 16;
        hb_audio_setfields(desc, 0x1800u);
        desc[VOICE_OFFSET / 4u] = 0;
        ((uint8_t *)desc)[PLAYING_OFFSET] = 0;
        if (i + 1u < BLOCKS)
            desc[0x54u / 4u] = (uint32_t)(uintptr_t)descriptors[i + 1u];
    }
    next_fill = PCM_SLOTS;
    refilled = missed = observed = highest = worst_fill_us = mixer_rate = 0;
    started_at = last_owned_at = hb_time_uptime_ms();
    hb_wake_lock(true);
    lv_label_set_text(status_label, "Playing a test tone for about 13 seconds.");
    if (!hb_audio_play_now(descriptors[0])) {
        lv_timer_delete(poll_timer);
        poll_timer = NULL;
        hb_wake_lock(false);
        hb_os_free(pcm);
        pcm = NULL;
        lv_label_set_text(status_label, "Could not open the stock mixer.");
    }
}

static int prepare_unload(void)
{
    return !poll_timer && (!previous_unload || previous_unload());
}

HB_APP_ENTRY(payload_entry)
{
    previous_unload = hb_shared->prepare_unload;
    hb_shared->prepare_unload = prepare_unload;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Audio Probe");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 50);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 208);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "Listen for breaks in the tone.");
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 115);

    lv_obj_t *button = lv_button_create(screen);
    lv_obj_set_size(button, 180, 44);
    lv_obj_align(button, LV_ALIGN_CENTER, 0, 55);
    lv_obj_add_event_cb(button, start_test, LV_EVENT_CLICKED, NULL);
    lv_obj_t *text = lv_label_create(button);
    lv_label_set_text(text, "Play test tone");
    lv_obj_center(text);
}
