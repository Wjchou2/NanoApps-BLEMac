#ifndef HB_CLASSIC_H
#define HB_CLASSIC_H

#include <stdbool.h>
#include <stdint.h>

#ifdef NANO_VIDEO_STREAM
/* A negative percentage means the Mac is working without a known total. */
bool hb_classic_video_progress(int *percent);
#endif

#ifdef NANO_YOUTUBE_SEARCH
#define CLASSIC_SEARCH_LIMIT 44u
#define CLASSIC_THUMB_SIDE 96u
#define CLASSIC_THUMB_BYTES (CLASSIC_THUMB_SIDE * CLASSIC_THUMB_SIDE * 2u)
#define CLASSIC_SONG_COUNT 8u
#define CLASSIC_SONG_TITLE 45u
struct classic_song_list {
    unsigned revision, count;
    char titles[CLASSIC_SONG_COUNT][CLASSIC_SONG_TITLE];
    char artists[CLASSIC_SONG_COUNT][CLASSIC_SONG_TITLE];
};
bool hb_classic_search_ready(void);
unsigned hb_classic_search_submit(const char *query);
unsigned hb_classic_search_play(unsigned revision, unsigned index);
bool hb_classic_search_results(struct classic_song_list *list);
unsigned hb_classic_search_mtu(void);
bool hb_classic_search_artwork(unsigned revision, unsigned index, uint16_t *pixels);
#ifdef NANO_YOUTUBE_AUDIO
#define CLASSIC_COVER_SIDE 160u
bool hb_classic_search_cover(uint16_t *pixels, bool refresh);
bool hb_classic_audio_playing(void);
bool hb_classic_audio_paused(void);
void hb_classic_audio_toggle_pause(void);
void hb_classic_audio_stop(void);
unsigned hb_classic_audio_elapsed_seconds(void);
#endif
#endif

#ifdef HB_CLASSIC_RAW
/* Called on the Bluetooth task. Copy the packet before returning and defer UI
 * work and sends to the UI task. The peer address is in display order. */
typedef void (*hb_classic_receive_fn)(const uint8_t *data, unsigned length, void *context);
unsigned hb_classic_listen(uint16_t psm, const uint8_t peer[6],
                           hb_classic_receive_fn receive, void *context);
bool hb_classic_connected(void);
unsigned hb_classic_outgoing_mtu(void);
/* Send one packet from the UI task. Returns zero on success. */
unsigned hb_classic_send(const void *bytes, unsigned length);
#else
/* Use the Mac sender protocol for the selected HB_CLASSIC media profile. */
unsigned hb_classic_listen_media(const uint8_t address[6]);
#endif
/* Optional for a new peer. Use pair_code/reply to confirm matching codes. */
void hb_classic_enable_pairing(void);
bool hb_classic_pair_code(unsigned *code);
bool hb_classic_pair_reply(unsigned code, bool confirm);
bool hb_classic_confirmed_peer(uint8_t address[6]);
/* Request teardown. Call prepare_unload on the UI task until it returns 1. */
void hb_classic_disconnect(void);
int hb_classic_prepare_unload(void);
bool hb_classic_busy(void);
/* For media profiles, also advances buffering, playback and file writes. */
const char *hb_classic_poll(void);

#ifdef NANO_STREAM
/* Initialize audio once at entry before installing the app's unload hook. */
void hb_classic_init_media(void);
#endif

#endif
