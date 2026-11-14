#include "hb_pcm.h"
#include "hb_prefs.h"
#include "hb_sdk.h"
#include "hb_shared.h"
#include "hb_wav.h"
#include "hb_classic_inbox.h"
#include "../entrain/platform/audio.h"
#include "lvgl/lvgl.h"
#include <string.h>

#define STARTUP_SILENCE_MS 600u

static lv_obj_t *status_label;
static lv_timer_t *playback_timer;
static int (*unload_audio)(void);
static uint32_t startup_silence;

static int prepare_unload(void)
{
    hb_wav_close();
    hb_wake_lock(false);
    return unload_audio();
}

static uint32_t pull_audio(int16_t *samples, uint32_t frames,
                           uint32_t advance_frames, void *context)
{
    (void)advance_frames;
    (void)context;
    uint32_t silence = startup_silence < frames ? startup_silence : frames;
    startup_silence -= silence;
    memset(samples, 0, silence * 4u);
    return silence + hb_wav_read(samples + silence * 2u, frames - silence);
}

static void poll_playback(lv_timer_t *timer)
{
    en_audio_tick();
    en_audio_state_t state = en_audio_state();
    if (state == EN_AUDIO_IDLE || state == EN_AUDIO_FAILED) {
        const char *status = state == EN_AUDIO_FAILED ? "Playback stopped."
                           : hb_wav_failed() ? "Could not finish reading this WAV."
                           : "Tap a file to play.";
        hb_wav_close();
        hb_wake_lock(false);
        lv_label_set_text(status_label, status);
        playback_timer = NULL;
        lv_timer_delete(timer);
    }
}

static void play_file(lv_event_t *event)
{
    if (playback_timer)
        return;

    lv_obj_t *list = lv_event_get_user_data(event);
    const char *name = lv_list_get_button_text(list, lv_event_get_target(event));
    char path[sizeof(CLASSIC_INBOX) + CLASSIC_FILENAME_CAPACITY];
    lv_snprintf(path, sizeof(path), CLASSIC_INBOX "/%s", name);
    uint32_t frames, rate;
    const char *error = hb_wav_open_native(path, CLASSIC_FILE_LIMIT, &frames, &rate);
    if (error) {
        lv_label_set_text(status_label, error);
        return;
    }

    startup_silence = rate * STARTUP_SILENCE_MS / 1000u;
    playback_timer = lv_timer_create(poll_playback, 20, NULL);
    if (!playback_timer || !en_audio_start_buffered_stream(rate, pull_audio, NULL)) {
        if (playback_timer)
            lv_timer_delete(playback_timer);
        playback_timer = NULL;
        hb_wav_close();
        lv_label_set_text(status_label, "Could not start playback.");
        return;
    }
    hb_wake_lock(true);
    lv_label_set_text_fmt(status_label, "Playing %s", name);
}

static bool add_files(lv_obj_t *list)
{
    hb_dir_t dir;
    if (!hb_fs_is_dir(CLASSIC_INBOX) || !hb_fs_dir_open(&dir, CLASSIC_INBOX, false))
        return false;

    bool found = false;
    bool is_dir;
    char name[CLASSIC_FILENAME_CAPACITY];
    char pending[CLASSIC_FILENAME_CAPACITY];
    bool has_pending = classic_inbox_pending_name(pending);
    lv_color_t surface = lv_color_hex(hb_color_surface());
    lv_color_t text = lv_color_hex(hb_color_text());
    lv_color_t divider = lv_color_hex(hb_color_bg());
    while (hb_fs_dir_next(&dir, name, sizeof(name), &is_dir)) {
        if (is_dir || !classic_inbox_wav_name(name) || (has_pending && !strcmp(name, pending)))
            continue;
        lv_obj_t *row = lv_list_add_button(list, LV_SYMBOL_PLAY, name);
        lv_obj_set_height(row, 44);
        lv_obj_set_style_bg_color(row, surface, 0);
        lv_obj_set_style_bg_color(row, divider, LV_STATE_PRESSED);
        lv_obj_set_style_text_color(row, text, 0);
        lv_obj_set_style_border_color(row, divider, 0);
        lv_obj_add_event_cb(row, play_file, LV_EVENT_CLICKED, list);
        found = true;
    }
    hb_fs_dir_close(&dir);
    return found;
}

HB_APP_ENTRY(payload_entry)
{
    lv_obj_t *screen = lv_screen_active();
    if (hb_shared) {
        en_audio_init();
        unload_audio = hb_shared->prepare_unload;
        hb_shared->prepare_unload = prepare_unload;
    }
    lv_obj_set_style_bg_color(screen, lv_color_hex(hb_color_bg()), 0);

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Audio Player");
    lv_obj_set_style_text_font(title, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(hb_color_text()), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 38);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 208);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(status_label, lv_color_hex(hb_color_text_dim()), 0);

    lv_obj_t *list = lv_list_create(screen);
    lv_obj_set_size(list, 208, 254);
    lv_obj_align(list, LV_ALIGN_TOP_MID, 0, 86);
    lv_obj_set_style_bg_color(list, lv_color_hex(hb_color_surface()), 0);
    if (hb_shared && add_files(list)) {
        lv_label_set_text(status_label, "Tap a file to play.");
        lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 352);
    } else {
        lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(status_label, "No saved audio. Send a clip with Bluetooth Test.");
        lv_obj_center(status_label);
    }
}
