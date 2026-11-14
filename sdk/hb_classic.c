/* Classic L2CAP file receiver for firmware 39A10023. */
#include "hb_classic.h"
#include "hb_classic_inbox.h"
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_bt_internal.h"
#include "lvgl/lvgl.h"
#include <string.h>

#define HEADER_SIZE 60u
#define NAME_SIZE 48u
#ifdef NANO_YOUTUBE_SEARCH
#define SONG_ID_BYTES 11u
#ifdef NANO_YOUTUBE_VIDEO
#define SEARCHING_STATUS "Searching YouTube..."
#define CHOOSE_STATUS "Choose a video."
#define SEARCH_STATUS "Search YouTube."
#define PLAY_REQUEST_STATUS "Waiting for the Mac to start the video..."
#define VIDEO_PLAY_RETRY_MS 1000u
#define VIDEO_PLAY_ATTEMPTS 4u
#else
#define SEARCHING_STATUS "Searching YouTube Music..."
#define CHOOSE_STATUS "Choose a song."
#define SEARCH_STATUS "Search YouTube Music."
#define DOWNLOAD_STATUS "Downloading selected song..."
#endif
#endif
#ifdef NANO_YOUTUBE_AUDIO
#include "hb_mulaw.h"
#include "hb_ima_adpcm.h"
#define COVER_BYTES (CLASSIC_COVER_SIDE * CLASSIC_COVER_SIDE * 2u)
#define AUDIO_CREDIT_WINDOW (320u * 1024u)
#define AUDIO_SAVE_RATE (40u * 1024u)
#define AUDIO_SAVE_BURST (8u * 1024u)
#define AUDIO_SAVE_WINDOW (24u * 1024u)
#define AUDIO_LIVE_STOP_WATERMARK (256u * 1024u)
#define AUDIO_LIVE_RESUME_WATERMARK (128u * 1024u)
#define MULAW_WAV_HEADER_SIZE 58u
#define IMA_WAV_HEADER_SIZE 60u
#define IMA_BLOCK_SIZE 1024u
#define AUDIO_TRACE_INTERVAL_MS 1000u
#define AUDIO_REPORT_INTERVAL_MS 1000u
#endif
#ifdef NANO_STREAM
#include "../apps/entrain/platform/audio.h"
#include "hb_shared.h"
#ifdef NANO_VIDEO_STREAM
#include "hb_h264.h"
#include "hb_video_surface.h"
/* The credit-controlled sender can fill a smaller ring without overrunning
 * it. Keep the original capacity for senders that don't honor credits. */
#define RECEIVE_CAPACITY (1024u * 1024u)
#define VIDEO_RECEIVE_CAPACITY (512u * 1024u)
#define VIDEO_CREDIT_WINDOW (VIDEO_RECEIVE_CAPACITY - 64u * 1024u)
_Static_assert(!(RECEIVE_CAPACITY & (RECEIVE_CAPACITY - 1u)) &&
               !(VIDEO_RECEIVE_CAPACITY & (VIDEO_RECEIVE_CAPACITY - 1u)), "Power-of-two video rings");
/* Must hold a whole group, including 12 frames of full stereo at 5 FPS. */
_Static_assert(VIDEO_CREDIT_WINDOW >= 12u + VIDEO_H264_MAX_FRAMES * (4u + VIDEO_H264_MAX_SAMPLE + 17640u),
               "Video credit must cover one maximum-size group");
#define VIDEO_SPOOL_WRITE_CHUNK (16u * 1024u)
#define VIDEO_CAPABILITY_REPEAT_COUNT 8u
#ifdef NANO_YOUTUBE_VIDEO
#define CLASSIC_PSM 0x100bu
#else
#define CLASSIC_PSM 0x1007u
#endif
#define MEDIA_HEADER_SIZE VIDEO_HEADER_BYTES
#define RECEIVE_LIMIT UINT32_MAX
#else
#ifdef NANO_YOUTUBE_AUDIO
#define CLASSIC_PSM 0x1009u
#define RECEIVE_LIMIT UINT32_MAX
#define RECEIVE_CAPACITY (384u * 1024u)
_Static_assert(AUDIO_CREDIT_WINDOW < RECEIVE_CAPACITY, "Audio credit must fit in the receive ring");
_Static_assert(AUDIO_LIVE_STOP_WATERMARK < RECEIVE_CAPACITY, "Leave room for in-flight audio");
#else
#define CLASSIC_PSM 0x1005u
#define RECEIVE_CAPACITY (1024u * 1024u)
#endif
#define MEDIA_HEADER_SIZE 44u
#endif
#else
#define CLASSIC_PSM 0x1003u
#define RECEIVE_CAPACITY (128u * 1024u)
#define DISK_WRITE_SIZE (32u * 1024u)
#endif
#ifndef RECEIVE_LIMIT
#define RECEIVE_LIMIT CLASSIC_FILE_LIMIT
#endif
#define PAIR_CREATE 0x080cd97fu
#define PAIR_DESTROY 0x080cda51u
#define PAIR_START 0x080cdac5u
#define PAIR_STOP 0x080cdae1u
#define PAIR_REPLY 0x080cd94bu
#define FIND_DEVICE 0x080ccd77u
#define PAIR_COMPLETE 3u
#define PAIR_AGENT_IN_USE 0xa3u
#define PAIR_MANAGER 0x08406b1du
#define PAIR_LOOKUP_AGENT 0x083a9311u
#define CONFIRM_VTABLE 0x087ec3a8u
#define CONFIRM_WORKER 0x08431b49u
#define COMPLETE_VTABLE 0x087ec224u
#define COMPLETE_WORKER 0x08430b61u
#define PAIR_WORKER_OFFSET 0x10u
#define THUMB_CB_NONZERO_BIT 0x0800u

extern const uint8_t classic_pair_capture_start[], classic_pair_capture_complete[];
extern const uint8_t classic_pair_capture_mailbox[], classic_pair_capture_end[];
extern const uint8_t classic_pair_capture_mode_branch[];

/* Layout shared with classic_pair_capture.S. */
struct pair_mailbox {
    uintptr_t agent, device;
    unsigned code, result;
};

static struct pair_mailbox local_pair;

typedef void (*guard_fn)(uintptr_t *);
typedef void (*pair_callback)(uintptr_t, unsigned, uintptr_t, unsigned, void *);
typedef void (*confirm_callback)(uintptr_t, uintptr_t, unsigned, unsigned, void *);
typedef void (*opened_fn)(unsigned, unsigned, unsigned, unsigned);
typedef void (*closed_fn)(unsigned, unsigned);
typedef void (*data_fn)(unsigned, const uint8_t *, unsigned);
typedef void (*incoming_fn)(unsigned, const uint8_t *, unsigned);
typedef unsigned (*listen_fn)(unsigned, incoming_fn);
typedef unsigned (*accept_fn)(opened_fn, closed_fn, data_fn, unsigned,
                              const void *, unsigned, const void *);

enum { CLOSED, LISTENING, OPENING, WAITING, RECEIVING, READY, DONE, FAILED };
#ifdef NANO_YOUTUBE_AUDIO
enum audio_receive_mode { AUDIO_RECEIVE_LIVE, AUDIO_RECEIVE_SAVED, AUDIO_RECEIVE_TEST };
#endif
static struct {
    unsigned state;
    unsigned open_error, close_reason;
    const char *failure;
    uint16_t cid, psm;
    unsigned out_mtu;
#ifdef NANO_VIDEO_STREAM
    unsigned native_video_announcements;
    uint32_t native_video_hello_ms;
    uint32_t video_prep_ms, video_total_bytes, video_estimated_frames;
    unsigned video_prep_stage, video_prep_percent;
#endif
    bool listening, stopping, confirmed;
    bool callbacks_installed;
    uint32_t retired_ms;
#ifdef HB_CLASSIC_RAW
    hb_classic_receive_fn receive;
    void *receive_context;
#endif
#ifdef NANO_YOUTUBE_SEARCH
    bool search_pending, awaiting_artwork;
    unsigned artwork_announcements;
    uint32_t search_started_ms;
    uint32_t artwork_hello_ms;
    char search_status[57];
    struct classic_song_list songs;
    char song_ids[CLASSIC_SONG_COUNT][SONG_ID_BYTES];
    char artist_id[SONG_ID_BYTES];
    char pending_artist[CLASSIC_SONG_TITLE];
    unsigned songs_expected, songs_received;
    unsigned artwork_remaining;
#ifdef NANO_YOUTUBE_AUDIO
    uint8_t *artwork;
    unsigned artwork_mask, artwork_index;
#endif
#endif
#ifdef NANO_YOUTUBE_AUDIO
    bool expects_cover, mac_paced_audio, audio_pause_sent;
    unsigned cover_received;
    uint8_t cover[COVER_BYTES];
    uint32_t credit_token, credit_limit, credit_attempt_ms, credit_started_ms;
    uint32_t audio_control_ms;
    unsigned credit_errors, credit_last_error;
    enum audio_receive_mode audio_mode;
#endif
    uint8_t peer[6];
    uint32_t size, received, written, checksum;
#ifdef NANO_STREAM
    uint32_t hash;
#endif
    char name[NAME_SIZE + 1];
#ifdef NANO_VIDEO_STREAM
    uint8_t *bytes;
    uint32_t capacity;
    bool progressive, video_flow_control;
    bool video_pause_sent;
    uint32_t video_credit_limit, video_credit_ms;
#ifdef NANO_YOUTUBE_VIDEO
    bool video_ready_announced;
    uint8_t video_play_request[4 + SONG_ID_BYTES];
    unsigned video_play_attempts;
    uint32_t video_play_attempt_ms;
#endif
#elif !defined(HB_CLASSIC_RAW)
    uint8_t bytes[RECEIVE_CAPACITY];
#endif
#if defined(NANO_VIDEO_STREAM) || defined(NANO_YOUTUBE_AUDIO)
    uint8_t header[HEADER_SIZE];
    uint32_t header_bytes;
#endif
} link;

#ifdef NANO_VIDEO_STREAM
#define RING_CAPACITY (link.capacity)
#else
#define RING_CAPACITY RECEIVE_CAPACITY
#endif

#ifndef HB_CLASSIC_RAW
static inline uint32_t ring_offset(uint32_t position)
{
#ifdef NANO_VIDEO_STREAM
    return position & (RING_CAPACITY - 1u);
#else
    return position % RING_CAPACITY;
#endif
}
#endif
/* Owned by the UI task. The radio callback only touches the ring. */
#ifdef NANO_STREAM
static struct {
    bool playing, verified;
    uint32_t rate, output_rate, stride, repeat, copies, startup_silence;
#ifndef NANO_VIDEO_STREAM
    bool spooling, reading;
    uint32_t audio_header, audio_end;
    uint8_t audio_cache[4096];
    uint32_t cache_start, cache_size;
#ifdef NANO_YOUTUBE_AUDIO
    bool mulaw;
    uint32_t trace_ms;
    uint32_t ima_frames_left;
    unsigned ima_channels, ima_samples_per_block, ima_cursor, ima_ready;
    uint8_t ima_block[IMA_BLOCK_SIZE];
    int16_t ima_pcm[2041u * 2u];
#endif
#endif
#ifdef NANO_VIDEO_STREAM
    uint32_t frame, sample, fps, side, channels, total_frames, header_size;
#endif
    int (*unload)(void);
} live;
#elif !defined(HB_CLASSIC_RAW)
static struct {
    bool open;
    uint32_t hash;
    char path[sizeof(CLASSIC_INBOX) + CLASSIC_FILENAME_CAPACITY];
} disk;
#endif
static char status[86];
static struct {
    struct pair_mailbox *events;
    unsigned start_error;
    const char *attach_failure;
    bool replied;
} pair = { .events = &local_pair };

static void lock(uintptr_t *guard) { ((guard_fn)0x083f0811u)(guard); }
static void unlock(uintptr_t *guard) { ((guard_fn)0x083f07f1u)(guard); }
static unsigned state(void) { return __atomic_load_n(&link.state, __ATOMIC_ACQUIRE); }
static void set_state(unsigned next) { __atomic_store_n(&link.state, next, __ATOMIC_RELEASE); }

static void pair_event(uintptr_t agent, unsigned event, uintptr_t device,
                       unsigned result, void *context)
{
    (void)context;
    if (event != PAIR_COMPLETE ||
        agent != __atomic_load_n(&local_pair.agent, __ATOMIC_ACQUIRE) ||
        device != __atomic_load_n(&local_pair.device, __ATOMIC_RELAXED))
        return;
    __atomic_store_n(&local_pair.result, result + 1u, __ATOMIC_RELEASE);
    __atomic_store_n(&local_pair.code, 0, __ATOMIC_RELEASE);
}

static void pair_request(uintptr_t agent, uintptr_t device, unsigned code,
                         unsigned numeric_comparison, void *context)
{
    (void)context;
    if (!numeric_comparison || code > 999999u ||
        agent != __atomic_load_n(&local_pair.agent, __ATOMIC_ACQUIRE) ||
        device != __atomic_load_n(&local_pair.device, __ATOMIC_RELAXED))
        return;
    __atomic_store_n(&local_pair.result, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&local_pair.code, code + 1u, __ATOMIC_RELEASE);
}

static struct pair_mailbox *system_pair_mailbox(void)
{
    if (*(const volatile uint32_t *)0x08431bc8u != CONFIRM_VTABLE ||
        *(const volatile uint32_t *)0x08430bd4u != COMPLETE_VTABLE ||
        *(const volatile uint32_t *)(CONFIRM_WORKER - 1u) != 0x460eb5feu ||
        *(const volatile uint32_t *)(COMPLETE_WORKER - 1u) != 0x460eb5feu)
        return NULL;

    uintptr_t start = (uintptr_t)classic_pair_capture_start;
    size_t complete_offset = (uintptr_t)classic_pair_capture_complete - start;
    size_t code_size = (uintptr_t)classic_pair_capture_mailbox - start;
    size_t size = (uintptr_t)classic_pair_capture_end - start;
    uintptr_t *confirm = (uintptr_t *)(CONFIRM_VTABLE + PAIR_WORKER_OFFSET);
    uintptr_t *complete = (uintptr_t *)(COMPLETE_VTABLE + PAIR_WORKER_OFFSET);
    uintptr_t confirm_worker = __atomic_load_n(confirm, __ATOMIC_ACQUIRE);
    uintptr_t complete_worker = __atomic_load_n(complete, __ATOMIC_ACQUIRE);
    struct pair_mailbox *previous = NULL;
    if (confirm_worker != CONFIRM_WORKER || complete_worker != COMPLETE_WORKER) {
        uintptr_t copy = confirm_worker & ~(uintptr_t)1u;
        if (!(confirm_worker & 1u) || copy < 0x08900000u || copy > 0x0c000000u - size ||
            complete_worker != ((copy + complete_offset) | 1u))
            return NULL;

        previous = (void *)(copy + code_size);
        const uint8_t *resident = (const uint8_t *)copy;
        if (!memcmp(resident, classic_pair_capture_start, code_size))
            return previous;

        /* The previous build differed by one branch bit. Replace only that
         * known copy and leave its code intact for any in-flight callbacks. */
        size_t branch_offset = (uintptr_t)classic_pair_capture_mode_branch - start;
        uint16_t old_branch, new_branch;
        memcpy(&old_branch, resident + branch_offset, sizeof(old_branch));
        memcpy(&new_branch, classic_pair_capture_mode_branch, sizeof(new_branch));
        size_t suffix = branch_offset + sizeof(old_branch);
        if (old_branch != (new_branch ^ THUMB_CB_NONZERO_BIT) ||
            memcmp(resident, classic_pair_capture_start, branch_offset) ||
            memcmp(resident + suffix, classic_pair_capture_start + suffix, code_size - suffix))
            return NULL;
    }

    uintptr_t copy = (uintptr_t)hb_os_alloc(size);
    if (!copy)
        return NULL;
    memcpy((void *)copy, classic_pair_capture_start, size);
    hb_icache_invalidate(copy, size);
    if (previous)
        __atomic_store_n(&previous->agent, 0, __ATOMIC_RELEASE);
    __atomic_store_n(complete, (copy + complete_offset) | 1u, __ATOMIC_RELEASE);
    __atomic_store_n(confirm, copy | 1u, __ATOMIC_RELEASE);
    return (void *)(copy + code_size);
}

static bool attach_system_pairing(uintptr_t agent, uintptr_t device)
{
    pair.attach_failure = "Could not find the active system pairing agent.";
    if (!agent || !device ||
        *(const volatile uint16_t *)(PAIR_MANAGER - 1u) != 0xb51cu ||
        *(const volatile uint16_t *)(PAIR_LOOKUP_AGENT - 1u) != 0xb57cu)
        return false;
    uintptr_t manager = ((uintptr_t (*)(void))PAIR_MANAGER)();
    if (!manager || !((uintptr_t (*)(uintptr_t, uintptr_t))PAIR_LOOKUP_AGENT)(manager, agent))
        return false;
    struct pair_mailbox *events = system_pair_mailbox();
    if (!events) {
        pair.attach_failure = "Could not load the pairing handler. Restart the nano.";
        return false;
    }
    __atomic_store_n(&events->code, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&events->result, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&events->device, device, __ATOMIC_RELAXED);
    __atomic_store_n(&events->agent, agent, __ATOMIC_RELEASE);
    pair.events = events;
    pair.attach_failure = NULL;
    return true;
}

static void start_pairing(uintptr_t session, uintptr_t system_agent)
{
    /* The stock Classic client registers a five-entry pairing callback table. */
    static const struct {
        pair_callback event;
        uintptr_t pin;
        confirm_callback confirm;
        uintptr_t display, reserved;
    } callbacks = { pair_event, 0, pair_request, 0, 0 };
    pair.start_error = 7;
    pair.attach_failure = NULL;
    if (!session || *(const volatile uint16_t *)(PAIR_CREATE - 1u) != 0xb5f8u ||
        *(const volatile uint16_t *)(PAIR_DESTROY - 1u) != 0xb510u ||
        *(const volatile uint16_t *)(PAIR_START - 1u) != 0xb510u ||
        *(const volatile uint16_t *)(PAIR_STOP - 1u) != 0xb510u ||
        *(const volatile uint16_t *)(PAIR_REPLY - 1u) != 0xb570u ||
        *(const volatile uint16_t *)(FIND_DEVICE - 1u) != 0xb57cu)
        return;

    /* The lookup reads a word and a halfword from the native address. */
    uint32_t address[2] = {0};
    memcpy(address, link.peer, sizeof(link.peer));
    uintptr_t device = 0, agent = 0;
    unsigned error = ((unsigned (*)(uintptr_t, const void *, uintptr_t *))FIND_DEVICE)(
        session, address, &device);
    if (!error)
        error = ((unsigned (*)(uintptr_t, const void *, void *, uintptr_t *))PAIR_CREATE)(
            session, &callbacks, NULL, &agent);
    if (error == PAIR_AGENT_IN_USE && attach_system_pairing(system_agent, device)) {
        pair.start_error = 0;
        return;
    }
    if (!error && agent) {
        __atomic_store_n(&local_pair.device, device, __ATOMIC_RELAXED);
        __atomic_store_n(&local_pair.agent, agent, __ATOMIC_RELEASE);
        error = ((unsigned (*)(uintptr_t))PAIR_START)(agent);
        if (error) {
            __atomic_store_n(&local_pair.agent, 0, __ATOMIC_RELEASE);
            ((void (*)(uintptr_t *))PAIR_DESTROY)(&agent);
        }
    }
    pair.start_error = error;
    if (!error && !agent)
        pair.start_error = 7;
}

void hb_classic_enable_pairing(void)
{
    if (!firmware_matches() || state() != LISTENING)
        return;
    uintptr_t model = read_pointer(BT_MODEL_SLOT);
    uintptr_t client = in_sdram(model, BT_CLIENT_OFFSET + sizeof(uintptr_t)) &&
                       read_pointer(model) == BT_MODEL_VTABLE ?
                       read_pointer(model + BT_CLIENT_OFFSET) : 0;
    if (!in_sdram(client, BT_CLIENT_PAIR_AGENT + sizeof(uintptr_t)) ||
        read_pointer(client) != BT_CLIENT_VTABLE ||
        read_pointer(client + BT_CLIENT_TYPE) != BT_CLASSIC_MODULE ||
        read_pointer(BT_CLIENT_VTABLE + BT_CLIENT_START_SLOT) != BT_CLIENT_START) {
        start_pairing(0, 0);
        return;
    }
    uintptr_t guard = 0;
    ((void (*)(uintptr_t *, void *))BT_CLIENT_LOCK)(&guard, (void *)(client + BT_CLIENT_MUTEX));
    start_pairing(read_pointer(client + BT_CLIENT_SESSION),
                  read_pointer(client + BT_CLIENT_PAIR_AGENT));
    ((void (*)(uintptr_t *))BT_CLIENT_UNLOCK)(&guard);
}

bool hb_classic_pair_code(unsigned *code)
{
    unsigned pending = __atomic_load_n(&pair.events->code, __ATOMIC_ACQUIRE);
    if (!pending)
        return false;
    *code = pending - 1u;
    return true;
}

bool hb_classic_pair_reply(unsigned code, bool confirm)
{
    struct pair_mailbox *events = pair.events;
    uintptr_t agent = __atomic_load_n(&events->agent, __ATOMIC_ACQUIRE);
    unsigned pending = code + 1u;
    if (!agent || !__atomic_compare_exchange_n(&events->code, &pending, 0, false,
                                              __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        return false;
    uintptr_t device = __atomic_load_n(&events->device, __ATOMIC_RELAXED);
    /* The stock UI passes 0 to accept and 1 to reject. */
    unsigned error = ((unsigned (*)(uintptr_t, uintptr_t, unsigned))PAIR_REPLY)(
        agent, device, confirm ? 0u : 1u);
    if (error)
        __atomic_store_n(&events->result, error + 1u, __ATOMIC_RELEASE);
    pair.replied = confirm && !error;
    return true;
}

#ifndef HB_CLASSIC_RAW
static uint32_t read_word(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

#endif

static void opened(unsigned cid, unsigned in_mtu, unsigned out_mtu, unsigned error)
{
    (void)in_mtu;
    link.out_mtu = out_mtu;
#ifdef NANO_VIDEO_STREAM
    link.native_video_announcements = 0;
    link.native_video_hello_ms = 0;
    link.video_prep_stage = 0;
    link.video_total_bytes = link.video_estimated_frames = 0;
    link.video_pause_sent = false;
#endif
#ifdef NANO_YOUTUBE_SEARCH
    link.songs.count = link.songs_expected = link.songs_received = 0;
    link.songs.revision++;
    link.artwork_remaining = 0;
#ifdef NANO_YOUTUBE_AUDIO
    link.artwork_mask = 0;
#endif
    link.awaiting_artwork = false;
    link.artwork_announcements = 0;
    link.artwork_hello_ms = 0;
    __atomic_store_n(&link.search_pending, false, __ATOMIC_RELEASE);
    link.search_status[0] = 0;
#endif
#ifdef NANO_YOUTUBE_AUDIO
    link.expects_cover = false;
    link.mac_paced_audio = link.audio_pause_sent = false;
    link.audio_control_ms = 0;
    link.audio_mode = AUDIO_RECEIVE_LIVE;
    link.cover_received = 0;
    link.credit_token = link.credit_limit = link.credit_attempt_ms = 0;
    link.credit_errors = link.credit_last_error = 0;
#endif
#ifdef NANO_YOUTUBE_VIDEO
    link.video_ready_announced = false;
    link.video_play_attempts = 0;
#endif
    link.cid = (uint16_t)cid;
    link.open_error = error;
    if (!link.stopping && !error)
        __atomic_store_n(&link.confirmed, true, __ATOMIC_RELEASE);
    set_state(link.stopping ? CLOSED : error ? FAILED : WAITING);
}

bool hb_classic_confirmed_peer(uint8_t address[6])
{
    if (!__atomic_load_n(&link.confirmed, __ATOMIC_ACQUIRE) &&
        !(pair.replied && __atomic_load_n(&pair.events->result, __ATOMIC_ACQUIRE) == 1))
        return false;
    memcpy(address, link.peer, sizeof(link.peer));
    return true;
}

#if defined(NANO_YOUTUBE_SEARCH) || defined(NANO_VIDEO_STREAM) || defined(HB_CLASSIC_RAW)
static bool sdu_send_supported(void)
{
    return *(const volatile uint32_t *)0x08418f94u == 0x41f0e92du &&
           *(const volatile uint32_t *)0x08415ac0u == 0x41ffe92du &&
           *(const volatile uint16_t *)0x08429f14u == 0xb570u;
}

/* Call with the radio lock held. The native path copies the packet. */
static unsigned send_sdu(const uint8_t *packet, unsigned packet_size)
{
    uintptr_t descriptor = ((uintptr_t (*)(const void *, unsigned, unsigned))0x08418f95u)(
        packet, packet_size, 0);
    if (!descriptor)
        return 104;
    unsigned error = ((unsigned (*)(unsigned, uint16_t, unsigned, uintptr_t, unsigned))0x08415ac1u)(
        0, link.cid, 0, descriptor, 0);
    ((void (*)(uintptr_t))0x08429f15u)(descriptor);
    return error;
}

#ifdef NANO_VIDEO_STREAM
/* Call with the radio lock held. Repeat after opening in case the Mac has
 * not attached its receive callback yet. */
static void announce_native_video(void)
{
    static const uint8_t capabilities[][4] = {
#ifdef HB_VIDEO_LAYER
        {'N', 'V', 'Q', '6'},
#endif
        {'N', 'V', 'Q', '5'}, {'N', 'V', 'Q', '4'}, {'N', 'V', 'Q', '3'}, {'N', 'V', 'P', '1'},
    };
    unsigned count = sizeof(capabilities) / sizeof(capabilities[0]);
    const uint8_t *capability = capabilities[link.native_video_announcements % count];
    link.native_video_hello_ms = hb_time_uptime_ms();
    if (!send_sdu(capability, sizeof(capabilities[0])))
        link.native_video_announcements++;
}

static bool video_preparation(const uint8_t *header)
{
    if (memcmp(header, "NVT1", 4))
        return false;
    unsigned stage = header[4], percent = header[5];
    if (stage >= 1 && stage <= 3 && (percent <= 100 || percent == 255)) {
        link.video_prep_ms = hb_time_uptime_ms();
        link.video_prep_percent = percent;
        link.video_prep_stage = stage;
#ifdef NANO_YOUTUBE_VIDEO
        link.search_started_ms = link.video_prep_ms;
        link.video_play_attempts = 0;
        __atomic_store_n(&link.search_pending, true, __ATOMIC_RELEASE);
#endif
    }
    return true;
}
#endif

#endif

#ifdef NANO_YOUTUBE_SEARCH
#ifdef NANO_YOUTUBE_VIDEO
static void reset_video_transfer(void);
#endif

bool hb_classic_search_ready(void)
{
    unsigned current = state();
    return link.cid && (current == WAITING || current == DONE) &&
#ifdef NANO_YOUTUBE_VIDEO
           hb_h264_can_receive() &&
#endif
           !__atomic_load_n(&link.search_pending, __ATOMIC_ACQUIRE);
}

unsigned hb_classic_search_mtu(void) { return link.out_mtu; }

static void reset_search_receive(void)
{
    link.received = link.written = link.size = 0;
    link.name[0] = 0;
    link.header_bytes = 0;
#ifdef NANO_YOUTUBE_VIDEO
    reset_video_transfer();
#endif
#ifdef NANO_YOUTUBE_AUDIO
    link.expects_cover = false;
    link.mac_paced_audio = link.audio_pause_sent = false;
    link.audio_control_ms = 0;
    link.audio_mode = AUDIO_RECEIVE_LIVE;
    link.cover_received = 0;
    link.credit_token = link.credit_limit = link.credit_attempt_ms = 0;
    link.credit_errors = link.credit_last_error = 0;
#endif
    live.rate = live.repeat = 0;
    live.verified = false;
}

#ifdef NANO_YOUTUBE_AUDIO
bool hb_classic_search_cover(uint16_t *pixels, bool refresh)
{
    uintptr_t guard = 0;
    lock(&guard);
    unsigned current = state();
    bool ready = (current == RECEIVING || current == READY) && link.expects_cover &&
                 link.cover_received == COVER_BYTES;
    if (ready && refresh)
        memcpy(pixels, link.cover, COVER_BYTES);
    unlock(&guard);
    return ready;
}

#endif

bool hb_classic_search_artwork(unsigned revision, unsigned index, uint16_t *pixels)
{
#ifdef NANO_YOUTUBE_AUDIO
    uintptr_t guard = 0;
    lock(&guard);
    bool ready = link.artwork && revision == link.songs.revision &&
                 index < link.songs.count && (link.artwork_mask & (1u << index));
    if (ready)
        memcpy(pixels, link.artwork + index * CLASSIC_THUMB_BYTES, CLASSIC_THUMB_BYTES);
    unlock(&guard);
    return ready;
#else
    (void)revision;
    (void)index;
    (void)pixels;
    return false;
#endif
}

/* Snapshot the same ACL lists and fields the firmware checks before sending. */
static unsigned search_acl_diagnostic(void)
{
    if (*(const volatile uint32_t *)0x08415c84u != 0x4906b148u ||
        *(const volatile uint32_t *)0x08415888u != 0x4906b148u)
        return 0;
    uintptr_t channel = 0;
    if (((unsigned (*)(unsigned, uintptr_t *))0x0841afe5u)(link.cid, &channel) ||
        channel < 0x08800000u || channel > 0x0c000000u - 0x2cu || (channel & 3u))
        return 1;
    uintptr_t acl = *(const uintptr_t *)(channel + 0x28u);
    if (((unsigned (*)(uintptr_t))0x08415c85u)(acl)) {
        const uint8_t *fields = (const uint8_t *)acl;
        if (fields[0x48] == 0xff)
            return 3;
        if (fields[0x16] == 6 || fields[0x16] == 7)
            return 4;
    } else if (!((unsigned (*)(uintptr_t))0x08415889u)(acl)) {
        return 2;
    }
    if (((unsigned (*)(uintptr_t))0x08415889u)(acl)) {
        const uint8_t *fields = (const uint8_t *)acl;
        if (fields[0x3b] == 6 || fields[0x3b] == 7)
            return 5;
    }
    return 6;
}

bool hb_classic_search_results(struct classic_song_list *list)
{
    uintptr_t guard = 0;
    lock(&guard);
    bool changed = list->revision != link.songs.revision;
    if (changed)
        *list = link.songs;
    unlock(&guard);
    return changed;
}

#ifdef NANO_YOUTUBE_AUDIO
static uint32_t audio_credit_limit(uint32_t consumed, uint32_t now)
{
    if (consumed >= link.size)
        return link.size;
    uint32_t remaining = link.size - consumed;
    bool paced = link.audio_mode != AUDIO_RECEIVE_LIVE;
    uint32_t window = paced ? AUDIO_SAVE_WINDOW : AUDIO_CREDIT_WINDOW;
    uint32_t limit = consumed + (remaining < window ? remaining : window);
    if (paced) {
        uint64_t paced = AUDIO_SAVE_BURST +
            (uint64_t)(now - link.credit_started_ms) * AUDIO_SAVE_RATE / 1000u;
        if (paced < limit)
            limit = (uint32_t)paced;
    }
    return limit;
}

static void grant_audio_credit(void)
{
    if (!link.credit_token || link.mac_paced_audio || !sdu_send_supported())
        return;
    uintptr_t guard = 0;
    lock(&guard);
    if (link.cid && state() == RECEIVING && (!link.out_mtu || link.out_mtu >= 12)) {
        uint32_t consumed = __atomic_load_n(&link.written, __ATOMIC_ACQUIRE);
        uint32_t now = hb_time_uptime_ms();
        uint32_t limit = audio_credit_limit(consumed, now);
        uint32_t elapsed = now - link.credit_attempt_ms;
        bool paced = link.audio_mode != AUDIO_RECEIVE_LIVE;
        bool advanced = limit > link.credit_limit &&
            (!link.credit_limit || limit == link.size ||
             limit - link.credit_limit >= (paced ? 16384u : 8192u));
        uint32_t min_interval = paced ? 250u : 100u;
        if ((!link.credit_attempt_ms || elapsed >= min_interval) && (advanced || elapsed >= 1000u)) {
            uint8_t packet[12] = {'N', 'Y', 'F', '1'};
            memcpy(packet + 4, &link.credit_token, sizeof(link.credit_token));
            memcpy(packet + 8, &limit, sizeof(limit));
            link.credit_attempt_ms = now;
            link.credit_last_error = send_sdu(packet, sizeof(packet));
            if (link.credit_last_error)
                link.credit_errors++;
            else
                link.credit_limit = limit;
        }
    }
    unlock(&guard);
}

static bool audio_sender_should_pause(uint32_t received, uint32_t consumed, bool playback_paused,
                                      bool sender_paused)
{
    uint32_t buffered = received >= consumed ? received - consumed : 0;
    uint32_t watermark = sender_paused ? AUDIO_LIVE_RESUME_WATERMARK : AUDIO_LIVE_STOP_WATERMARK;
    return playback_paused || buffered >= watermark;
}

static void update_audio_sender_pause(void)
{
    if (!link.mac_paced_audio)
        return;
    uint32_t received = __atomic_load_n(&link.received, __ATOMIC_ACQUIRE);
    uint32_t consumed = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
    bool pause = audio_sender_should_pause(received, consumed, hb_classic_audio_paused(),
                                           link.audio_pause_sent);
    if (pause == link.audio_pause_sent || !sdu_send_supported())
        return;
    uint32_t now = hb_time_uptime_ms();
    if (link.audio_control_ms && now - link.audio_control_ms < 250u)
        return;
    uintptr_t guard = 0;
    lock(&guard);
    if (link.cid && state() == RECEIVING && link.mac_paced_audio) {
        uint8_t packet[8] = {'N', 'Y', 'Z', pause ? '2' : '3'};
        memcpy(packet + 4, &link.credit_token, sizeof(link.credit_token));
        link.audio_control_ms = now;
        if (!send_sdu(packet, sizeof(packet)))
            link.audio_pause_sent = pause;
    }
    unlock(&guard);
}

#endif

static unsigned submit_search_packet(uint8_t *packet, unsigned packet_size, bool playing,
                                     unsigned revision, unsigned index)
{
    if (!sdu_send_supported())
        return 1;
    uintptr_t guard = 0;
    lock(&guard);
    if (!hb_classic_search_ready() || (link.out_mtu && packet_size > link.out_mtu)) {
        unlock(&guard);
        return 2;
    }
    if (playing) {
        if (revision != link.songs.revision || index >= link.songs.count) {
            unlock(&guard);
            return 101;
        }
        memcpy(packet + 4, link.song_ids[index], SONG_ID_BYTES);
    }
    unsigned error = send_sdu(packet, packet_size);
    unsigned diagnostic = error == 0x19eu ? search_acl_diagnostic() : 0;
    unsigned mtu = link.out_mtu;
    if (!error) {
        link.search_started_ms = hb_time_uptime_ms();
        reset_search_receive();
        __atomic_store_n(&link.search_pending, true, __ATOMIC_RELEASE);
        if (playing) {
#ifdef NANO_YOUTUBE_VIDEO
            memcpy(link.video_play_request, packet, sizeof(link.video_play_request));
            link.video_play_attempts = 1;
            link.video_play_attempt_ms = link.search_started_ms;
            memcpy(link.search_status, PLAY_REQUEST_STATUS, sizeof(PLAY_REQUEST_STATUS));
#else
            memcpy(link.search_status, DOWNLOAD_STATUS, sizeof(DOWNLOAD_STATUS));
#endif
        } else {
            link.songs.count = link.songs_expected = link.songs_received = 0;
            link.songs.revision++;
#ifdef NANO_YOUTUBE_AUDIO
            link.artwork_mask = 0;
#endif
            memcpy(link.search_status, SEARCHING_STATUS, sizeof(SEARCHING_STATUS));
        }
        set_state(WAITING);
    }
    unlock(&guard);
    if (error == 0x19eu) {
        char report[112];
        int count = lv_snprintf(report, sizeof(report),
                                "Classic send 0x19E\nACL check: %u\nOutgoing MTU: %u\nPacket bytes: %u\n",
                                diagnostic, mtu, packet_size);
        if (count > 0 && (size_t)count < sizeof(report))
            hb_fs_write("/Data/YouTubeAudio-link.txt", report, (uint32_t)count);
    }
    return error;
}

unsigned hb_classic_search_submit(const char *query)
{
    size_t length = strlen(query);
    if (!length || length > CLASSIC_SEARCH_LIMIT)
        return 101;
    uint8_t packet[4 + CLASSIC_SEARCH_LIMIT];
    memcpy(packet, "NYQ1", 4);
    memcpy(packet + 4, query, length);
    return submit_search_packet(packet, 4u + (unsigned)length, false, 0, 0);
}

unsigned hb_classic_search_play(unsigned revision, unsigned index)
{
    uint8_t packet[4 + SONG_ID_BYTES] = {'N', 'Y', 'P', '1'};
    return submit_search_packet(packet, sizeof(packet), true, revision, index);
}

#ifdef NANO_YOUTUBE_VIDEO
/* Call with the radio lock held. Either a status record or the video
 * header ends retries. A missing acknowledgement restores the picker. */
static void poll_video_play_request(uint32_t now)
{
    if (!link.cid || state() != WAITING || !link.video_play_attempts ||
        now - link.video_play_attempt_ms < VIDEO_PLAY_RETRY_MS)
        return;
    if (link.video_play_attempts < VIDEO_PLAY_ATTEMPTS) {
        link.video_play_attempts++;
        link.video_play_attempt_ms = now;
        send_sdu(link.video_play_request, sizeof(link.video_play_request));
    } else {
        link.video_play_attempts = 0;
        memcpy(link.search_status, "Mac did not confirm playback. Tap Play to retry.",
               sizeof("Mac did not confirm playback. Tap Play to retry."));
        __atomic_store_n(&link.search_pending, false, __ATOMIC_RELEASE);
    }
}
#endif

static bool search_response(const uint8_t *header)
{
    if (!memcmp(header, "NYE1", 4)) {
        if (link.awaiting_artwork && link.songs_received == link.songs_expected) {
            link.awaiting_artwork = false;
            link.songs.count = link.songs_received;
            link.songs.revision++;
            link.search_status[0] = 0;
            __atomic_store_n(&link.search_pending, false, __ATOMIC_RELEASE);
        }
        return true;
    }
    if (!memcmp(header, "NYI1", 4)) {
        /* Video results use text only. Drain thumbnail bytes from senders
         * that still include them so the next search packet stays aligned. */
        link.artwork_remaining = CLASSIC_THUMB_BYTES;
#ifdef NANO_YOUTUBE_AUDIO
        link.artwork_index = header[4];
        if (link.artwork_index < link.songs_expected && !link.artwork)
            link.artwork = hb_os_alloc(CLASSIC_SONG_COUNT * CLASSIC_THUMB_BYTES);
#endif
        return true;
    }
    if (!memcmp(header, "NYL1", 4)) {
        link.songs_expected = header[4] <= CLASSIC_SONG_COUNT ? header[4] : 0;
        link.songs_received = 0;
#ifdef NANO_YOUTUBE_AUDIO
        link.artwork_mask = 0;
#endif
        link.awaiting_artwork = header[5] == 1 && link.songs_expected;
        link.songs.count = 0;
        link.songs.revision++;
        link.pending_artist[0] = 0;
        if (!link.songs_expected) {
            memcpy(link.search_status, "No results found.", sizeof("No results found."));
            __atomic_store_n(&link.search_pending, false, __ATOMIC_RELEASE);
        } else {
            link.search_started_ms = hb_time_uptime_ms();
            __atomic_store_n(&link.search_pending, true, __ATOMIC_RELEASE);
        }
        return true;
    }
    if (!memcmp(header, "NYT1", 4)) {
        memcpy(link.artist_id, header + 4, SONG_ID_BYTES);
        memcpy(link.pending_artist, header + 4 + SONG_ID_BYTES, CLASSIC_SONG_TITLE - 1);
        link.pending_artist[CLASSIC_SONG_TITLE - 1] = 0;
        return true;
    }
    if (!memcmp(header, "NYR1", 4)) {
        if (link.songs_received < link.songs_expected) {
            unsigned index = link.songs_received++;
            memcpy(link.song_ids[index], header + 4, SONG_ID_BYTES);
            memcpy(link.songs.titles[index], header + 4 + SONG_ID_BYTES, CLASSIC_SONG_TITLE - 1);
            link.songs.titles[index][CLASSIC_SONG_TITLE - 1] = 0;
            link.songs.artists[index][0] = 0;
            if (link.pending_artist[0] && !memcmp(link.artist_id, link.song_ids[index], SONG_ID_BYTES))
                memcpy(link.songs.artists[index], link.pending_artist, CLASSIC_SONG_TITLE);
            link.pending_artist[0] = 0;
            if (link.songs_received == link.songs_expected && !link.awaiting_artwork) {
                link.songs.count = link.songs_received;
                link.songs.revision++;
                link.search_status[0] = 0;
                __atomic_store_n(&link.search_pending, false, __ATOMIC_RELEASE);
            }
        }
        return true;
    }
    if (memcmp(header, "NYS1", 4) && memcmp(header, "NYA1", 4))
        return false;
    memcpy(link.search_status, header + 4, 56);
    link.search_status[56] = 0;
    link.search_started_ms = hb_time_uptime_ms();
#ifdef NANO_YOUTUBE_VIDEO
    link.video_play_attempts = 0;
#endif
    if (header[2] == 'S')
        link.songs_expected = link.songs_received = 0;
#ifdef NANO_YOUTUBE_VIDEO
    if (header[2] == 'S')
        link.video_prep_stage = 0;
#endif
    __atomic_store_n(&link.search_pending, header[2] == 'A', __ATOMIC_RELEASE);
    return true;
}
#endif

static void closed(unsigned cid, unsigned reason)
{
    if (link.cid != cid)
        return;
    link.cid = 0;
    link.close_reason = reason;
    unsigned current = state();
#ifdef NANO_VIDEO_STREAM
    if (current == RECEIVING && link.progressive) {
        set_state(READY);
        return;
    }
#endif
    if (current != READY && current != DONE && current != FAILED)
        set_state(CLOSED);
}

static void received(unsigned cid, const uint8_t *data, unsigned length)
{
    if (link.cid != cid)
        return;
#ifdef HB_CLASSIC_RAW
    if (state() == WAITING && link.receive && length)
        link.receive(data, length, link.receive_context);
#else
    unsigned current = state();
#ifdef NANO_YOUTUBE_SEARCH
    if (current == DONE && length) {
        reset_search_receive();
        link.search_status[0] = 0;
        current = WAITING;
        set_state(current);
    }
#endif
    if (current == WAITING) {
#ifdef NANO_YOUTUBE_SEARCH
next_search_header:
        if (link.artwork_remaining) {
            unsigned take = link.artwork_remaining < length ? link.artwork_remaining : length;
#ifdef NANO_YOUTUBE_AUDIO
            unsigned index = link.artwork_index;
            if (link.artwork && index < link.songs_expected) {
                unsigned offset = index * CLASSIC_THUMB_BYTES + CLASSIC_THUMB_BYTES - link.artwork_remaining;
                memcpy(link.artwork + offset, data, take);
                if (take == link.artwork_remaining)
                    link.artwork_mask |= 1u << index;
            }
#endif
            link.artwork_remaining -= take;
            data += take;
            length -= take;
            if (!length)
                return;
        }
#endif
        const uint8_t *header = data;
#if defined(NANO_VIDEO_STREAM) || defined(NANO_YOUTUBE_AUDIO)
        uint32_t take = HEADER_SIZE - link.header_bytes;
        if (take > length)
            take = length;
        memcpy(link.header + link.header_bytes, data, take);
        link.header_bytes += take;
        if (link.header_bytes < HEADER_SIZE)
            return;
        header = link.header;
        data += take;
        length -= take;
#endif
#ifdef NANO_VIDEO_STREAM
        if (video_preparation(header)) {
            link.header_bytes = 0;
            return;
        }
#endif
#ifdef NANO_YOUTUBE_SEARCH
        if (search_response(header)) {
            link.header_bytes = 0;
            if (length)
                goto next_search_header;
            return;
        }
#endif
#ifdef NANO_YOUTUBE_VIDEO
        if (!hb_h264_can_receive()) {
            link.failure = "Close the current video before sending another.";
            set_state(FAILED);
            return;
        }
#endif
#ifdef NANO_YOUTUBE_AUDIO
        bool paced_cover = !memcmp(header, "NYC3", 4);
        bool paced_audio = !memcmp(header, "NYW3", 4);
        link.mac_paced_audio = paced_cover || paced_audio;
        bool flow_cover = paced_cover || !memcmp(header, "NYC2", 4);
        bool flow_audio = paced_audio || !memcmp(header, "NYW2", 4);
        link.expects_cover = flow_cover || !memcmp(header, "NYC1", 4);
        if (flow_cover || flow_audio)
            link.credit_token = read_word(header + HEADER_SIZE - 4u);
#endif
#ifdef NANO_VIDEO_STREAM
        bool video_flow_control = !memcmp(header, "NCL3", 4);
        link.video_prep_stage = 0;
        link.video_pause_sent = false;
        link.video_total_bytes = video_flow_control ? read_word(header + 4) : 0;
        link.video_estimated_frames = video_flow_control ? read_word(header + 8) : 0;
        if (video_flow_control || !memcmp(header, "NCL2", 4)) {
            link.progressive = true;
            link.video_flow_control = video_flow_control;
            link.video_credit_limit = link.video_credit_ms = 0;
            link.size = UINT32_MAX;
        } else
#endif
        {
            if (
#if !defined(NANO_VIDEO_STREAM) && !defined(NANO_YOUTUBE_AUDIO)
                length != HEADER_SIZE ||
#endif
                (memcmp(header, "NCL1", 4)
#ifdef NANO_YOUTUBE_AUDIO
                 && !link.expects_cover && !flow_audio
#endif
                ) || read_word(header + 4) > RECEIVE_LIMIT) {
#ifdef NANO_VIDEO_STREAM
                lv_snprintf(status, sizeof(status), "Unknown Classic header %02X%02X%02X%02X.",
                            header[0], header[1], header[2], header[3]);
                link.failure = status;
#else
                link.failure = "Classic file header invalid.";
#endif
                set_state(FAILED);
                return;
            }
            link.size = read_word(header + 4);
            link.checksum = read_word(header + 8);
#ifdef NANO_STREAM
            link.hash = 2166136261u;
#endif
            unsigned name_size = NAME_SIZE;
#ifdef NANO_YOUTUBE_AUDIO
            if (flow_cover || flow_audio)
                name_size -= 4u;
#endif
            memset(link.name, 0, sizeof(link.name));
            for (unsigned i = 0; i < name_size; i++) {
                uint8_t c = header[12 + i];
                if (!c)
                    break;
                bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
                link.name[i] = safe ? c : '_';
            }
            if (!link.name[0] || link.name[0] == '.')
                link.name[0] = '_';
#ifdef NANO_YOUTUBE_AUDIO
            size_t name_length = strlen(link.name);
            const char *suffix = name_length >= 4 ? link.name + name_length - 4 : "";
            if (!strcmp(suffix, ".nsa"))
                link.audio_mode = AUDIO_RECEIVE_SAVED;
            else if (!strcmp(suffix, ".ntx"))
                link.audio_mode = AUDIO_RECEIVE_TEST;
            link.credit_started_ms = hb_time_uptime_ms();
#endif
        }
#ifdef NANO_YOUTUBE_SEARCH
        __atomic_store_n(&link.search_pending, false, __ATOMIC_RELEASE);
#endif
#ifdef NANO_YOUTUBE_VIDEO
        link.video_ready_announced = false;
        link.video_play_attempts = 0;
#endif
#ifdef NANO_VIDEO_STREAM
        uint32_t capacity = link.video_flow_control ? VIDEO_RECEIVE_CAPACITY : RECEIVE_CAPACITY;
        if (link.bytes && link.capacity != capacity) {
            hb_os_free(link.bytes);
            link.bytes = NULL;
        }
        if (!link.bytes) {
            if (hb_os_heap_largest() >= capacity + 128u * 1024u)
                link.bytes = hb_os_alloc(capacity);
            if (!link.bytes) {
                link.failure = "Not enough free memory to receive a video.";
                set_state(FAILED);
                return;
            }
            link.capacity = capacity;
        }
#endif
        current = link.size ? RECEIVING : READY;
        set_state(current);
#if !defined(NANO_VIDEO_STREAM) && !defined(NANO_YOUTUBE_AUDIO)
        return;
#endif
    }
#if defined(NANO_VIDEO_STREAM) || defined(NANO_YOUTUBE_AUDIO)
    if (current == RECEIVING && length) {
#else
    if (current == RECEIVING) {
#endif
#ifdef NANO_YOUTUBE_AUDIO
        if (link.expects_cover && link.cover_received < COVER_BYTES) {
            unsigned take = COVER_BYTES - link.cover_received;
            if (take > length)
                take = length;
            memcpy(link.cover + link.cover_received, data, take);
            link.cover_received += take;
            data += take;
            length -= take;
            if (!length)
                return;
        }
#endif
        uint32_t received = __atomic_load_n(&link.received, __ATOMIC_RELAXED);
        uint32_t written = __atomic_load_n(&link.written, __ATOMIC_ACQUIRE);
        if (!length || length > link.size - received) {
            link.failure = "Classic file length mismatch.";
            set_state(FAILED);
            return;
        }
        if (written > received || received - written > RING_CAPACITY ||
            length > RING_CAPACITY - (received - written)) {
#ifdef NANO_STREAM
            link.failure = "Playback buffer full. Reconnect to retry.";
#else
            link.failure = "Disk fell behind the transfer. Reconnect to retry.";
#endif
            set_state(FAILED);
            return;
        }
        uint32_t start = ring_offset(received);
        uint32_t first = RING_CAPACITY - start;
        if (first > length)
            first = length;
        memcpy(link.bytes + start, data, first);
        if (first < length)
            memcpy(link.bytes, data + first, length - first);
#ifdef NANO_STREAM
#ifdef NANO_VIDEO_STREAM
        if (!link.progressive)
#endif
        for (uint32_t i = 0; i < length; i++)
            link.hash = (link.hash ^ data[i]) * 16777619u;
#endif
        received += length;
        __atomic_store_n(&link.received, received, __ATOMIC_RELEASE);
        if (received == link.size
#ifdef NANO_VIDEO_STREAM
            && !link.progressive
#endif
        )
            set_state(READY);
    }
#endif
}

static void incoming(unsigned psm, const uint8_t *address, unsigned cid)
{
    if (psm != link.psm || state() != LISTENING || !address ||
        memcmp(address, link.peer, sizeof(link.peer)))
        return;
    link.cid = (uint16_t)cid;
    set_state(OPENING);
    uint32_t config[6] = {672u, 0x03041388u, 0, 0, 0, 0};
    unsigned error = ((accept_fn)0x083d0fb5u)(opened, closed, received, cid,
                                               config, 1, (const void *)0x0877eb70u);
    if (error) {
        link.open_error = error;
        set_state(FAILED);
    }
}

#ifdef NANO_STREAM
void hb_classic_init_media(void)
{
    en_audio_init();
    live.unload = hb_shared->prepare_unload;
}

static int prepare_media_unload(void)
{
    if (live.unload && !live.unload())
        return 0;
    if (live.playing) {
        hb_wake_lock(false);
#ifdef NANO_VIDEO_STREAM
        video_stream_reset();
#endif
    }
    live.playing = false;
    return 1;
}

static int16_t live_sample(uint32_t offset)
{
    uint32_t start = ring_offset(offset);
#ifdef NANO_YOUTUBE_AUDIO
    if (live.mulaw)
        return hb_mulaw_decode(link.bytes[start]);
#endif
    return (int16_t)(link.bytes[start] |
                     (uint16_t)link.bytes[ring_offset(start + 1u)] << 8);
}

#ifdef NANO_VIDEO_STREAM
static bool parse_live_header(void)
{
    const uint8_t *header = link.bytes;
    uint32_t frames = read_word(header + 4);
    bool current = !memcmp(header, "NVS2", 4);
    if (!current && memcmp(header, "NVS1", 4))
        return false;
    uint32_t header_size = current ? VIDEO_LIVE_HEADER_BYTES : VIDEO_HEADER_BYTES;
    uint32_t fps = current ? read_word(header + 8) : VIDEO_FPS;
    uint32_t side = current ? read_word(header + 12) : VIDEO_SIDE;
    uint32_t rate = current ? read_word(header + 16) : VIDEO_WIRE_RATE;
    uint32_t channels = current ? read_word(header + 20) : 1;
    if (!frames || !hb_video_frame_rate_supported(fps) ||
        side < 16 || side > VIDEO_LIVE_MAX_SIDE ||
        (rate != 7350 && rate != 11025 && rate != 22050 && rate != 44100) ||
        (channels != 1 && channels != 2))
        return false;
    uint64_t size = header_size + (uint64_t)frames * side * side +
                    (uint64_t)frames * rate / fps * channels * 2u;
    if (size != link.size)
        return false;
    live.rate = rate;
    live.output_rate = rate == 44100 ? 44100 : 22050;
    live.repeat = live.output_rate / rate;
    live.fps = fps;
    live.side = side;
    live.channels = channels;
    live.header_size = header_size;
    live.total_frames = frames;
    live.frame = live.sample = 0;
    return true;
}

static uint32_t read_live_samples(int16_t *pcm, uint32_t frames)
{
    uint32_t end = __atomic_load_n(&link.received, __ATOMIC_ACQUIRE);
    uint32_t pos = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
    uint32_t produced = 0;
    uint32_t pixels = live.side * live.side;
    while (produced < frames && live.frame < live.total_frames) {
        uint64_t index = live.frame;
        uint32_t audio_frames = (index + 1u) * live.rate / live.fps - index * live.rate / live.fps;
        uint32_t record = pixels + audio_frames * live.channels * 2u;
        if (end - pos < record)
            break;
        if (!live.sample) {
            uint8_t *picture = video_stream_frame(live.frame);
            uint32_t start = ring_offset(pos);
            uint32_t first = RING_CAPACITY - start;
            if (first > pixels)
                first = pixels;
            memcpy(picture, link.bytes + start, first);
            if (first < pixels)
                memcpy(picture + first, link.bytes, pixels - first);
        }
        uint32_t count = audio_frames * live.repeat - live.sample;
        if (count > frames - produced)
            count = frames - produced;
        for (uint32_t i = 0; i < count; i++) {
            uint32_t offset = pos + pixels + (live.sample++ / live.repeat) * live.channels * 2u;
            int16_t left = live_sample(offset);
            *pcm++ = left;
            *pcm++ = live.channels == 2 ? live_sample(offset + 2) : left;
        }
        produced += count;
        if (live.sample == audio_frames * live.repeat) {
            live.sample = 0;
            live.frame++;
            pos += record;
        }
    }
    __atomic_store_n(&link.written, pos, __ATOMIC_RELEASE);
    return produced || pos == link.size ? produced : EN_AUDIO_PULL_WAIT;
}
#else
static uint32_t audio_header_size(uint32_t received)
{
#ifdef NANO_YOUTUBE_AUDIO
    if (received >= MEDIA_HEADER_SIZE) {
        if (link.bytes[20] == 7u)
            return MULAW_WAV_HEADER_SIZE;
        if (link.bytes[20] == 0x11u)
            return IMA_WAV_HEADER_SIZE;
    }
#else
    (void)received;
#endif
    return MEDIA_HEADER_SIZE;
}

static bool parse_live_header(void)
{
    const uint8_t *wav = link.bytes;
    unsigned header_size = MEDIA_HEADER_SIZE;
    unsigned sample_bytes = 2u;
    uint32_t audio_size = link.size > header_size ? link.size - header_size : 0;
    if (memcmp(wav, "RIFF", 4) || memcmp(wav + 8, "WAVEfmt ", 8) ||
        read_word(wav + 4) != link.size - 8 || wav[21] || wav[23] || wav[35])
        return false;
#ifdef NANO_YOUTUBE_AUDIO
    live.ima_channels = 0;
    if (wav[20] == 0x11u) {
        unsigned channels = wav[22];
        uint32_t rate = read_word(wav + 24);
        if ((link.audio_mode != AUDIO_RECEIVE_SAVED &&
             (!link.mac_paced_audio || link.audio_mode != AUDIO_RECEIVE_LIVE)) ||
            link.size <= IMA_WAV_HEADER_SIZE || (channels != 1 && channels != 2) ||
            (rate != 7350u && rate != 11025u && rate != 22050u && rate != 44100u) ||
            read_word(wav + 16) != 20u ||
            read_word(wav + 32) != (IMA_BLOCK_SIZE | (4u << 16)) ||
            memcmp(wav + 40, "fact", 4) || read_word(wav + 44) != 4u ||
            memcmp(wav + 52, "data", 4))
            return false;
        unsigned samples = (IMA_BLOCK_SIZE - 4u * channels) * 2u / channels + 1u;
        uint32_t frames = read_word(wav + 48);
        uint32_t length = read_word(wav + 56);
        if (read_word(wav + 36) != (2u | (samples << 16)) ||
            length != link.size - IMA_WAV_HEADER_SIZE || !frames ||
            ((uint64_t)frames + samples - 1u) / samples * IMA_BLOCK_SIZE != length)
            return false;
        live.mulaw = false;
        live.ima_channels = channels;
        live.ima_frames_left = frames;
        live.ima_samples_per_block = samples;
        live.ima_cursor = live.ima_ready = 0;
        live.rate = rate;
        live.output_rate = rate == 44100u ? 44100u : 22050u;
        live.copies = live.output_rate / live.rate;
        live.repeat = 0;
        live.audio_header = IMA_WAV_HEADER_SIZE;
        live.audio_end = link.size;
        return true;
    }
    live.mulaw = wav[20] == 7u;
    if (live.mulaw) {
        header_size = MULAW_WAV_HEADER_SIZE;
        sample_bytes = 1u;
        audio_size = read_word(wav + 54);
        if (link.size <= header_size || read_word(wav + 16) != 18u || wav[34] != 8u ||
            wav[36] || wav[37] || memcmp(wav + 38, "fact", 4) ||
            read_word(wav + 42) != 4u || memcmp(wav + 50, "data", 4) ||
            audio_size > link.size - header_size ||
            link.size - header_size - audio_size != (audio_size & 1u))
            return false;
    } else
#endif
    if (link.size <= header_size || wav[20] != 1u || wav[34] != 16u ||
        read_word(wav + 16) != 16u || memcmp(wav + 36, "data", 4) ||
        read_word(wav + 40) != audio_size)
        return false;
    unsigned channels = wav[22];
    uint32_t rate = read_word(wav + 24);
    if ((channels != 1 && channels != 2) ||
        (rate != 7350u && rate != 11025u && rate != 22050u && rate != 44100u))
        return false;
    live.stride = channels * sample_bytes;
    if (wav[32] != live.stride || wav[33] || read_word(wav + 28) != rate * live.stride ||
        audio_size % live.stride)
        return false;
    live.rate = rate;
    live.output_rate = rate == 44100u ? 44100u : 22050u;
    live.copies = live.output_rate / live.rate;
    live.audio_header = header_size;
    live.audio_end = header_size + audio_size;
    return true;
}

#ifdef NANO_YOUTUBE_AUDIO
static uint32_t audio_wire_rate(void)
{
    if (live.ima_channels)
        return (live.rate * IMA_BLOCK_SIZE + live.ima_samples_per_block - 1u) /
               live.ima_samples_per_block;
    return live.rate * live.stride;
}

static bool decode_ima_block(const uint8_t *block)
{
    unsigned channels = live.ima_channels;
    for (unsigned ch = 0; ch < channels; ch++) {
        const uint8_t *header = block + 4u * ch;
        int predictor = hb_ima_predictor(header);
        int index = header[2];
        if (index > 88)
            return false;
        live.ima_pcm[ch] = (int16_t)predictor;
        unsigned frame = 1;
        for (unsigned group = 4u * channels; group < IMA_BLOCK_SIZE; group += 4u * channels) {
            const uint8_t *codes = block + group + 4u * ch;
            for (unsigned i = 0; i < 4; i++) {
                live.ima_pcm[2u * frame++ + ch] = hb_ima_sample(codes[i] & 15u, &predictor, &index);
                live.ima_pcm[2u * frame++ + ch] = hb_ima_sample(codes[i] >> 4, &predictor, &index);
            }
        }
    }
    if (channels == 1)
        for (unsigned frame = 0; frame < live.ima_samples_per_block; frame++)
            live.ima_pcm[2u * frame + 1u] = live.ima_pcm[2u * frame];
    return true;
}

static uint32_t read_ima(int16_t *pcm, uint32_t frames)
{
    uint32_t produced = 0;
    while (produced < frames) {
        if (live.ima_cursor == live.ima_ready) {
            if (!live.ima_frames_left)
                break;
            uint32_t pos = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
            if (live.reading) {
                if (hb_fs_reader_read(live.ima_block, IMA_BLOCK_SIZE) != IMA_BLOCK_SIZE) {
                    link.failure = "Could not read saved ADPCM audio.";
                    set_state(FAILED);
                    break;
                }
            } else {
                uint32_t end = __atomic_load_n(&link.received, __ATOMIC_ACQUIRE);
                if (end < pos || end - pos < IMA_BLOCK_SIZE)
                    return produced ? produced : EN_AUDIO_PULL_WAIT;
                uint32_t start = pos % RECEIVE_CAPACITY;
                unsigned first = RECEIVE_CAPACITY - start;
                if (first > IMA_BLOCK_SIZE)
                    first = IMA_BLOCK_SIZE;
                memcpy(live.ima_block, link.bytes + start, first);
                if (first < IMA_BLOCK_SIZE)
                    memcpy(live.ima_block + first, link.bytes, IMA_BLOCK_SIZE - first);
            }
            if (!decode_ima_block(live.ima_block)) {
                link.failure = "Invalid ADPCM audio block.";
                set_state(FAILED);
                break;
            }
            live.ima_cursor = 0;
            live.ima_ready = live.ima_frames_left < live.ima_samples_per_block ?
                live.ima_frames_left : live.ima_samples_per_block;
            live.ima_frames_left -= live.ima_ready;
            __atomic_store_n(&link.written, pos + IMA_BLOCK_SIZE, __ATOMIC_RELEASE);
        }
        if (live.copies != 1u) {
            *pcm++ = live.ima_pcm[2u * live.ima_cursor];
            *pcm++ = live.ima_pcm[2u * live.ima_cursor + 1u];
            if (++live.repeat == live.copies) {
                live.repeat = 0;
                live.ima_cursor++;
            }
            produced++;
            continue;
        }
        unsigned count = live.ima_ready - live.ima_cursor;
        if (count > frames - produced)
            count = frames - produced;
        memcpy(pcm, live.ima_pcm + live.ima_cursor * 2u, count * 4u);
        pcm += count * 2u;
        live.ima_cursor += count;
        produced += count;
    }
    return produced;
}
#endif

#ifdef NANO_YOUTUBE_AUDIO
enum audio_report_stage {
    AUDIO_RECEIVING = 1, AUDIO_SAVING, AUDIO_STARTING_LIVE, AUDIO_STARTING_SAVED,
    AUDIO_PLAYING_LIVE, AUDIO_PLAYING_SAVED, AUDIO_TEST_RECEIVING, AUDIO_FINISHED, AUDIO_STOPPED,
    AUDIO_FAILED, AUDIO_DISCONNECTED
};

static struct {
    uint32_t last_ms;
    unsigned sequence;
    enum audio_report_stage stage;
    bool backup;
    size_t used;
    char text[4096];
} audio_report;

static void report_audio_progress(enum audio_report_stage stage)
{
    static const char *const labels[] = {
        [AUDIO_RECEIVING] = "receiving",
        [AUDIO_SAVING] = "saving",
        [AUDIO_STARTING_LIVE] = "starting live",
        [AUDIO_STARTING_SAVED] = "starting saved",
        [AUDIO_PLAYING_LIVE] = "playing live",
        [AUDIO_PLAYING_SAVED] = "playing saved",
        [AUDIO_TEST_RECEIVING] = "receive test",
        [AUDIO_FINISHED] = "finished",
        [AUDIO_STOPPED] = "stopped",
        [AUDIO_FAILED] = "failed",
        [AUDIO_DISCONNECTED] = "disconnected",
    };
    uint32_t now = hb_time_uptime_ms();
    if (stage == audio_report.stage &&
        (stage == AUDIO_TEST_RECEIVING || stage >= AUDIO_FINISHED ||
         now - audio_report.last_ms < AUDIO_REPORT_INTERVAL_MS))
        return;
    audio_report.stage = stage;
    audio_report.last_ms = now;
    uint32_t received = __atomic_load_n(&link.received, __ATOMIC_ACQUIRE);
    uint32_t consumed = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
    unsigned queued, mixer_mask;
    en_audio_queue_status(&queued, &mixer_mask);
    char line[320];
    int size = lv_snprintf(line, sizeof(line),
        "#%u %u ms %s transfer=%08x rx=%u/%u consumed=%u ring=%u "
        "credit=%u rate=%u elapsed=%us audio=%u queue=%u mixer=%x buffering=%u "
        "link=%u mode=%u close=%x heap=%u largest=%u credit_errors=%u last=%x %.55s\n",
        ++audio_report.sequence, (unsigned)now, labels[stage], (unsigned)link.credit_token,
        (unsigned)received, (unsigned)link.size, (unsigned)consumed,
        (unsigned)(received >= consumed ? received - consumed : 0),
        (unsigned)link.credit_limit, (unsigned)live.rate,
        live.playing ? (unsigned)en_audio_elapsed() : 0u,
        (unsigned)en_audio_state(), queued, mixer_mask, (unsigned)en_audio_stream_buffering(),
        state(), (unsigned)link.audio_mode, link.close_reason,
        (unsigned)hb_os_heap_free(), (unsigned)hb_os_heap_largest(),
        link.credit_errors, link.credit_last_error,
        stage == AUDIO_FAILED && link.failure ? link.failure : "");
    if (size <= 0 || (unsigned)size >= sizeof(line))
        return;
    while (audio_report.used + (unsigned)size >= sizeof(audio_report.text)) {
        size_t first = (size_t)(strchr(audio_report.text, '\n') - audio_report.text) + 1u;
        audio_report.used -= first;
        memmove(audio_report.text, audio_report.text + first, audio_report.used);
    }
    memcpy(audio_report.text + audio_report.used, line, (unsigned)size);
    audio_report.used += (unsigned)size;
    audio_report.text[audio_report.used] = '\0';
    /* Alternate synced snapshots so a reboot during one write leaves the
       previous snapshot in the other file. This uses the one-shot writer,
       independent of the open audio spool. */
    const char *path = audio_report.backup ? "/Data/YouTubeAudio-playback-backup.txt" :
                                            "/Data/YouTubeAudio-playback.txt";
    if (hb_fs_write(path, audio_report.text, audio_report.used))
        audio_report.backup = !audio_report.backup;
}
#endif

#define STREAM_AUDIO_PATH "/Data/.Stream-audio.tmp"

static void close_audio_spool(void)
{
    if (live.spooling) {
        hb_fs_stream_close();
        live.spooling = false;
        hb_fs_remove(STREAM_AUDIO_PATH);
        hb_wake_lock(false);
    }
    if (live.reading) {
        hb_fs_reader_close();
        live.reading = false;
        hb_fs_remove(STREAM_AUDIO_PATH);
    }
}

#ifdef NANO_YOUTUBE_AUDIO
bool hb_classic_audio_playing(void) { return live.playing; }

bool hb_classic_audio_paused(void) { return en_audio_state() == EN_AUDIO_PAUSED; }

void hb_classic_audio_toggle_pause(void)
{
    if (live.playing)
        en_audio_set_stream_paused(!hb_classic_audio_paused());
}

unsigned hb_classic_audio_elapsed_seconds(void)
{
    return live.playing ? (unsigned)en_audio_elapsed() : 0;
}

void hb_classic_audio_stop(void)
{
    if (!live.playing)
        return;
    report_audio_progress(AUDIO_STOPPED);
    en_audio_shutdown();
    close_audio_spool();
    live.playing = false;
    hb_wake_lock(false);
    if (state() == READY)
        set_state(DONE);
    else
        hb_classic_disconnect();
}
#endif

static uint32_t pull_saved_audio(int16_t *pcm, uint32_t frames, uint32_t advance, void *context)
{
    (void)advance;
    (void)context;
#ifdef NANO_YOUTUBE_AUDIO
    if (live.ima_channels)
        return read_ima(pcm, frames);
#endif
    uint32_t pos = link.written;
    uint32_t produced = 0;
    while (produced < frames && pos < live.audio_end) {
        if (pos == live.cache_start + live.cache_size) {
            live.cache_start = pos;
            uint32_t count = live.audio_end - pos;
            if (count > sizeof(live.audio_cache))
                count = sizeof(live.audio_cache);
            live.cache_size = hb_fs_reader_read(live.audio_cache, count);
            if (live.cache_size != count) {
                live.cache_size = 0;
                link.failure = "Could not read saved audio.";
                set_state(FAILED);
                break;
            }
        }
        const uint8_t *sample = live.audio_cache + pos - live.cache_start;
#ifdef NANO_YOUTUBE_AUDIO
        if (live.mulaw) {
            *pcm++ = hb_mulaw_decode(sample[0]);
            *pcm++ = hb_mulaw_decode(sample[live.stride - 1u]);
        } else
#endif
        {
            const uint8_t *right = sample + live.stride - 2u;
            *pcm++ = (int16_t)(sample[0] | (unsigned)sample[1] << 8);
            *pcm++ = (int16_t)(right[0] | (unsigned)right[1] << 8);
        }
        produced++;
        if (++live.repeat == live.copies) {
            live.repeat = 0;
            pos += live.stride;
        }
    }
    link.written = pos;
    return produced;
}

static const char *spool_audio(uint32_t received)
{
    if (!live.spooling && !live.reading) {
#ifdef NANO_YOUTUBE_AUDIO
        report_audio_progress(AUDIO_RECEIVING);
#endif
        if (received < audio_header_size(received))
            return state() == READY ? "Incomplete audio header." : "Waiting for audio...";
        if (!parse_live_header()) {
            link.failure = "Unsupported audio format.";
            set_state(FAILED);
#ifdef NANO_YOUTUBE_AUDIO
            report_audio_progress(AUDIO_FAILED);
#endif
            return link.failure;
        }
        if (!(hb_fs_is_dir("/Data") || hb_fs_mkdir("/Data")) ||
            !hb_fs_stream_open(STREAM_AUDIO_PATH))
            goto failed;
        live.spooling = true;
        hb_trace_log("ASave", link.size, live.rate);
        hb_wake_lock(true);
        __atomic_store_n(&link.written, live.audio_header, __ATOMIC_RELEASE);
    }
    if (!live.reading) {
        uint32_t written = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
        uint32_t started = hb_time_uptime_ms();
        while (written < received) {
            uint32_t start = written % RECEIVE_CAPACITY;
            uint32_t count = received - written;
            if (count > RECEIVE_CAPACITY - start)
                count = RECEIVE_CAPACITY - start;
            if (count > 16u * 1024u)
                count = 16u * 1024u;
            if (!hb_fs_stream_write(link.bytes + start, count))
                goto failed;
            written += count;
            __atomic_store_n(&link.written, written, __ATOMIC_RELEASE);
            if (hb_time_uptime_ms() - started >= 8u)
                break;
        }
        if (!live.verified || written != link.size) {
#ifdef NANO_YOUTUBE_AUDIO
            report_audio_progress(AUDIO_SAVING);
#endif
            return "Receiving audio...";
        }
        bool closed = hb_fs_stream_close();
        live.spooling = false;
        if (!closed || hb_fs_size(STREAM_AUDIO_PATH) != link.size - live.audio_header ||
            !hb_fs_reader_open(STREAM_AUDIO_PATH)) {
            hb_fs_remove(STREAM_AUDIO_PATH);
            hb_wake_lock(false);
            goto failed;
        }
        live.reading = true;
        live.cache_start = live.audio_header;
        live.cache_size = 0;
        link.written = live.audio_header;
#ifdef NANO_YOUTUBE_AUDIO
        report_audio_progress(AUDIO_STARTING_SAVED);
#endif
        if (!en_audio_start_buffered_stream(live.output_rate, pull_saved_audio, NULL)) {
            hb_wake_lock(false);
            goto failed;
        }
        live.playing = true;
        hb_trace_log("APlay", link.size, live.rate);
    }
    en_audio_tick();
#ifdef NANO_YOUTUBE_AUDIO
    report_audio_progress(AUDIO_PLAYING_SAVED);
#endif
    if (state() == FAILED || en_audio_state() == EN_AUDIO_FAILED) {
        en_audio_shutdown();
        live.playing = false;
        hb_wake_lock(false);
        goto failed;
    }
    if (en_audio_state() != EN_AUDIO_IDLE)
        return "Playing audio...";
#ifdef NANO_YOUTUBE_AUDIO
    report_audio_progress(AUDIO_FINISHED);
#endif
    close_audio_spool();
    live.playing = false;
    hb_wake_lock(false);
    set_state(DONE);
    return "Audio finished.";

failed:
    close_audio_spool();
    link.failure = "Could not save or play the audio.";
    set_state(FAILED);
#ifdef NANO_YOUTUBE_AUDIO
    report_audio_progress(AUDIO_FAILED);
#endif
    return link.failure;
}

static uint32_t read_live_samples(int16_t *pcm, uint32_t frames)
{
#ifdef NANO_YOUTUBE_AUDIO
    if (live.ima_channels)
        return read_ima(pcm, frames);
#endif
    uint32_t end = __atomic_load_n(&link.received, __ATOMIC_ACQUIRE);
    uint32_t pos = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
    uint32_t audio_end = live.audio_end;
    if (pos >= audio_end)
        return 0;
    if (end > audio_end)
        end = audio_end;
    if (end <= pos)
        return EN_AUDIO_PULL_WAIT;
    uint32_t available = ((end - pos) / live.stride) * live.copies;
    if (available <= live.repeat)
        return EN_AUDIO_PULL_WAIT;
    available -= live.repeat;
    if (frames > available)
        frames = available;
    unsigned right_offset = live.stride - 2u;
#ifdef NANO_YOUTUBE_AUDIO
    if (live.mulaw)
        right_offset = live.stride - 1u;
#endif
    for (uint32_t i = 0; i < frames; i++) {
        *pcm++ = live_sample(pos);
        *pcm++ = live_sample(pos + right_offset);
        if (++live.repeat == live.copies) {
            live.repeat = 0;
            pos += live.stride;
        }
    }
    __atomic_store_n(&link.written, pos, __ATOMIC_RELEASE);
    return frames;
}
#endif

static uint32_t pull_live(int16_t *pcm, uint32_t frames, uint32_t advance, void *context)
{
    (void)advance;
    (void)context;
    uint32_t silence = live.startup_silence < frames ? live.startup_silence : frames;
    live.startup_silence -= silence;
    memset(pcm, 0, silence * 4u);
    if (frames == silence)
        return silence;
    uint32_t samples = read_live_samples(pcm + silence * 2u, frames - silence);
    if (samples == EN_AUDIO_PULL_WAIT)
        return silence ? silence : EN_AUDIO_PULL_WAIT;
    return silence + samples;
}

static const char *live_failure(const char *message)
{
    uintptr_t guard = 0;
    lock(&guard);
#ifdef NANO_VIDEO_STREAM
    if (link.cid && sdu_send_supported()) {
        uint8_t packet[60] = {'N', 'V', 'F', '1'};
        unsigned capacity = link.out_mtu && link.out_mtu < sizeof(packet) ? link.out_mtu : sizeof(packet);
        if (capacity > 5) {
            unsigned length = 4;
            while (length + 1 < capacity && message[length - 4]) {
                packet[length] = (uint8_t)message[length - 4];
                length++;
            }
            send_sdu(packet, length + 1);
        }
    }
#endif
    link.failure = message;
    set_state(FAILED);
    unlock(&guard);
#ifdef NANO_YOUTUBE_AUDIO
    report_audio_progress(AUDIO_FAILED);
#endif
    prepare_media_unload();
    return message;
}

#ifdef NANO_VIDEO_STREAM
static bool h264_streaming;
static bool video_spooling;
static bool video_prep_awake;
static uint32_t video_received_group_frames;

bool hb_classic_video_progress(int *percent)
{
    uintptr_t guard = 0;
    lock(&guard);
    unsigned current = state();
    if (current == WAITING && link.video_prep_stage) {
        *percent = link.video_prep_percent == 255 ? -1 : (int)link.video_prep_percent;
        unlock(&guard);
        return true;
    }
    if (current != RECEIVING || !link.progressive) {
        unlock(&guard);
        return false;
    }
    uint32_t total = link.video_total_bytes;
    uint32_t done = __atomic_load_n(&link.received, __ATOMIC_ACQUIRE);
    if (!total) {
        total = link.video_estimated_frames;
        done = video_received_group_frames;
    }
    unlock(&guard);
    *percent = total ? (done >= total ? 99 : (int)((uint64_t)done * 100u / total)) : -1;
    return true;
}

static void grant_video_credit(void)
{
    if (!sdu_send_supported())
        return;
    uintptr_t guard = 0;
    lock(&guard);
    if (link.cid && state() == RECEIVING && (!link.out_mtu || link.out_mtu >= 8)) {
        uint32_t written = __atomic_load_n(&link.written, __ATOMIC_ACQUIRE);
        uint32_t window = VIDEO_CREDIT_WINDOW;
        uint32_t limit = written + (link.size - written < window ? link.size - written : window);
        uint32_t now = hb_time_uptime_ms();
        uint32_t elapsed = now - link.video_credit_ms;
        if (!link.video_credit_ms || elapsed >= 1000u ||
            (elapsed >= 50u && limit - link.video_credit_limit >= 4096u)) {
            uint8_t packet[8] = {'N', 'Y', 'G', '1'};
            memcpy(packet + 4, &limit, sizeof(limit));
            link.video_credit_ms = now;
            if (!send_sdu(packet, sizeof(packet)))
                link.video_credit_limit = limit;
        }
    }
    unlock(&guard);
}

static void stop_h264_stream(void)
{
    if (h264_streaming) {
        hb_h264_live_stop();
        h264_streaming = false;
    }
}

typedef struct { uint32_t pos, end; } live_cursor;

static bool read_live_ring(void *buffer, uint32_t length, void *context)
{
    live_cursor *cursor = context;
    if (length > cursor->end - cursor->pos)
        return false;
    uint32_t start = ring_offset(cursor->pos);
    uint32_t first = RING_CAPACITY - start;
    if (first > length)
        first = length;
    memcpy(buffer, link.bytes + start, first);
    if (length > first)
        memcpy((uint8_t *)buffer + first, link.bytes, length - first);
    cursor->pos += length;
    return true;
}

static const char *h264_stream_status(void)
{
    if (hb_h264_live_paused())
        return "Video paused.";
    if (hb_h264_live_playing() && !en_audio_stream_buffering())
        return "Playing H.264 video...";
    uint32_t buffered = hb_h264_live_buffer_ms();
    lv_snprintf(status, sizeof(status), "Buffering H.264... %u.%u s ready",
                (unsigned)(buffered / 1000u), (unsigned)(buffered / 100u % 10u));
    return status;
}

static void update_video_sender_pause(void)
{
    bool paused = hb_h264_live_paused();
    if (paused == link.video_pause_sent || !sdu_send_supported())
        return;
    uintptr_t guard = 0;
    lock(&guard);
    if (link.cid && !send_sdu((const uint8_t *)(paused ? "NYZ1" : "NYZ0"), 4))
        link.video_pause_sent = paused;
    unlock(&guard);
}

static const char *poll_h264_stream(uint32_t received)
{
    const char *failure = "Incomplete H.264 video group.";
    live_cursor cursor = {__atomic_load_n(&link.written, __ATOMIC_RELAXED), received};
    if (!h264_streaming) {
        const uint32_t header_size = 24;
        if (received < header_size)
            return live.verified ? live_failure("Incomplete H.264 stream header.") : "Buffering video...";
        uint32_t size = read_word(link.bytes + 4);
        uint32_t count = read_word(link.bytes + 8);
        if (count > link.size / 10u || !size || size > VIDEO_H264_MAX_AVCC)
            return live_failure("Invalid H.264 stream configuration.");
        if (received < header_size + size)
            return live.verified ? live_failure("Incomplete H.264 configuration.") : "Buffering video...";
        if (!hb_h264_live_begin(link.bytes + header_size, size, count,
                                   read_word(link.bytes + 12), read_word(link.bytes + 16),
                                   read_word(link.bytes + 20), VIDEO_H264_MAX_FRAMES))
            return live_failure(hb_h264_live_error());
        h264_streaming = true;
        cursor.pos = header_size + size;
        __atomic_store_n(&link.written, cursor.pos, __ATOMIC_RELEASE);
    }
    if (hb_h264_live_cancelled()) {
#ifdef NANO_YOUTUBE_VIDEO
        if (live.verified) {
            stop_h264_stream();
            set_state(DONE);
            return "Video stopped.";
        }
#endif
        hb_classic_disconnect();
        return "Video stopped.";
    }
    update_video_sender_pause();
    if (hb_h264_live_room()) {
        uint8_t word[4];
        if (read_live_ring(word, sizeof(word), &cursor)) {
            uint32_t length = read_word(word);
            if (!length || length > RING_CAPACITY - sizeof(word)) {
                failure = "Invalid H.264 video group size.";
                goto invalid;
            }
            if (length <= received - cursor.pos) {
                cursor.end = cursor.pos + length;
                const char *error = hb_h264_live_group(read_live_ring, &cursor, length);
                if (error || cursor.pos != cursor.end) {
                    failure = error ? error : "H.264 video group length mismatch.";
                    goto invalid;
                }
                __atomic_store_n(&link.written, cursor.pos, __ATOMIC_RELEASE);
            } else if (live.verified) {
                goto invalid;
            }
        } else if (live.verified) {
            goto invalid;
        }
    }
    if (live.verified && hb_h264_live_complete() &&
        __atomic_load_n(&link.written, __ATOMIC_RELAXED) != link.size) {
        failure = "Unexpected data after the last video group.";
        goto invalid;
    }

    int result = hb_h264_live_tick();
    if (result < 0) {
        failure = hb_h264_live_error();
        goto invalid;
    }
    if (result > 0 && live.verified) {
        stop_h264_stream();
        set_state(DONE);
        return "Video finished.";
    }
    return h264_stream_status();

invalid:
    stop_h264_stream();
    return live_failure(failure);
}

static void discard_video_spool(void)
{
    if (video_spooling) {
        hb_fs_stream_close();
        hb_fs_remove(VIDEO_H264_RECEIVE_PATH);
        video_spooling = false;
        hb_wake_lock(false);
    }
}

static struct {
    uint8_t config[VIDEO_H264_MAX_AVCC];
    uint32_t config_size, fps, rate, channels, frames;
    unsigned group_frames;
    bool ended, partial;
} progressive;

#ifdef NANO_YOUTUBE_VIDEO
static void reset_video_transfer(void)
{
    link.progressive = link.video_flow_control = false;
    memset(&progressive, 0, sizeof(progressive));
}
#endif

static uint32_t ring_hash(uint32_t start, uint32_t length)
{
    uint32_t hash = 2166136261u;
    while (length--) {
        hash = (hash ^ link.bytes[ring_offset(start)]) * 16777619u;
        start++;
    }
    return hash;
}

static bool spool_ring(uint32_t pos, uint32_t length)
{
    while (length) {
        uint32_t start = ring_offset(pos);
        uint32_t count = RING_CAPACITY - start;
        if (count > length)
            count = length;
        if (count > VIDEO_SPOOL_WRITE_CHUNK)
            count = VIDEO_SPOOL_WRITE_CHUNK;
        if (!hb_fs_stream_write(link.bytes + start, count))
            return false;
        pos += count;
        length -= count;
    }
    return true;
}

static const char *poll_progressive(uint32_t received, bool closed)
{
    if (link.video_flow_control && !closed)
        grant_video_credit();
    const char *failure = "Mac disconnected before finishing the video.";
    uint32_t written = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
    if (!written) {
        video_received_group_frames = 0;
        if (received < 20 && closed)
            goto invalid;
        if (received < 20)
            return "Waiting for the first video group...";
        const uint8_t *header = link.bytes;
        uint32_t size = read_word(header + 4);
        bool small_groups = header[3] == '3';
        bool play_live = !memcmp(header, small_groups ? "NVL3" : "NVL2", 4);
        failure = "Invalid H.264 stream configuration.";
        if ((!play_live && memcmp(header, small_groups ? "NVD3" : "NVD2", 4)) ||
            !size || size > VIDEO_H264_MAX_AVCC)
            goto invalid;
        if (received < 20 + size && closed) {
            failure = "Mac disconnected during video setup.";
            goto invalid;
        }
        if (received < 20 + size)
            return "Waiting for the video configuration...";
        progressive.config_size = size;
        progressive.fps = read_word(header + 8);
        progressive.rate = read_word(header + 12);
        progressive.channels = read_word(header + 16);
        unsigned channels = progressive.channels & ~VIDEO_H264_AUDIO_ADPCM;
        if (channels < 1 || channels > 2)
            goto invalid;
        progressive.group_frames = small_groups ? VIDEO_H264_NATIVE_GROUP_FRAMES : VIDEO_H264_MAX_FRAMES;
        memcpy(progressive.config, header + 20, size);
        if (play_live) {
            if (!hb_h264_live_begin(progressive.config, size, 0, progressive.fps,
                                       progressive.rate, progressive.channels, progressive.group_frames)) {
                failure = hb_h264_live_error();
                goto invalid;
            }
            h264_streaming = true;
        } else {
            failure = "Could not open temporary video storage.";
            hb_trace_log("VSpool", link.video_total_bytes, progressive.group_frames);
            if (!hb_h264_can_receive() ||
                !(hb_fs_is_dir("/Data") || hb_fs_mkdir("/Data")) ||
                !hb_fs_stream_open(VIDEO_H264_RECEIVE_PATH))
                goto invalid;
            video_spooling = true;
            hb_wake_lock(true);
        }
        written = 20 + size;
        __atomic_store_n(&link.written, written, __ATOMIC_RELEASE);
    }

    failure = "Mac disconnected before finishing the video.";
    if (h264_streaming && hb_h264_live_cancelled()) {
#ifdef NANO_YOUTUBE_VIDEO
        if (progressive.ended) {
            stop_h264_stream();
            set_state(DONE);
            return "Video stopped.";
        }
#endif
        hb_classic_disconnect();
        return "Video stopped.";
    }
    if (h264_streaming)
        update_video_sender_pause();
    bool converting = h264_streaming && hb_h264_live_group_pending();
    if (converting) {
        const char *error = hb_h264_live_advance();
        if (error) {
            failure = error;
            goto invalid;
        }
    }
    /* A saved transfer can drain several groups per UI tick. Keep a time
       budget so flash I/O still yields regularly to touch and Bluetooth.
       For live playback, each poll either decodes a group or converts one frame. */
    uint32_t batch_started = hb_time_uptime_ms();
    for (unsigned groups = 0; !converting && !progressive.ended && received - written >= 12; groups++) {
        if (groups && (!video_spooling || groups >= 8 || hb_time_uptime_ms() - batch_started >= 8u))
            break;
        uint8_t header[12];
        live_cursor cursor = {written, received};
        read_live_ring(header, sizeof(header), &cursor);
        uint32_t length = read_word(header);
        uint32_t count = read_word(header + 4);
        uint32_t checksum = read_word(header + 8);
        if (!length) {
            failure = "Invalid H.264 end marker.";
            if (checksum || count != progressive.frames || !count || received != cursor.pos)
                goto invalid;
            if (link.video_total_bytes && cursor.pos != link.video_total_bytes)
                goto invalid;
            progressive.ended = true;
            /* The end marker is the last byte the player needs from the ring. */
            uintptr_t guard = 0;
            lock(&guard);
            set_state(READY);
            uint8_t *buffer = link.bytes;
            link.bytes = NULL;
            unlock(&guard);
            hb_os_free(buffer);
            __atomic_store_n(&link.written, cursor.pos, __ATOMIC_RELEASE);
            if (video_spooling) {
                hb_trace_log("VSpDone", cursor.pos, count);
                bool closed = hb_fs_stream_close();
                video_spooling = false;
                hb_wake_lock(false);
                uint32_t expected = cursor.pos - 20u - progressive.config_size - 12u
                                    - ((count - 1u) / progressive.group_frames + 1u) * 4u;
                if (!closed || hb_fs_size(VIDEO_H264_RECEIVE_PATH) != expected) {
                    hb_fs_remove(VIDEO_H264_RECEIVE_PATH);
                    goto invalid;
                }
                const char *error = hb_h264_play_groups(count, progressive.fps, progressive.rate,
                    progressive.channels, progressive.config, progressive.config_size, progressive.group_frames);
                if (error)
                    return live_failure(error);
                set_state(DONE);
                return "Video received.";
            }
        } else {
            failure = "Invalid H.264 video group header.";
            if (!count || count > progressive.group_frames || progressive.partial ||
                length > progressive.group_frames * (VIDEO_H264_MAX_SAMPLE + 4u + 17640u))
                goto invalid;
            if (length > received - cursor.pos && closed) {
                failure = "Mac disconnected during a video group.";
                goto invalid;
            }
            /* A display refresh can temporarily make a decoded slot unsafe
               to reuse. Service playback before checking room for this group. */
            if (h264_streaming && length <= received - cursor.pos &&
                hb_h264_live_playing() && hb_h264_live_tick() < 0) {
                failure = hb_h264_live_error();
                goto invalid;
            }
            if (length <= received - cursor.pos && (!h264_streaming || hb_h264_live_room())) {
                failure = "H.264 video group checksum mismatch.";
                if (ring_hash(cursor.pos, length) != checksum)
                    goto invalid;
                failure = "Could not decode or save the H.264 video group.";
                if (h264_streaming) {
                    if (count < progressive.group_frames &&
                        !hb_h264_live_set_final_count(progressive.frames + count))
                        goto invalid;
                    cursor.end = cursor.pos + length;
                    const char *error = hb_h264_live_queue(read_live_ring, &cursor, length);
                    if (error || cursor.pos != cursor.end) {
                        failure = error ? error : "H.264 video group length mismatch.";
                        goto invalid;
                    }
                } else if (!spool_ring(written, 8) || !spool_ring(cursor.pos, length)) {
                    failure = "Could not save the video. Check free space on the nano.";
                    goto invalid;
                } else {
                    if (!progressive.frames || (written >> 20) != ((cursor.pos + length) >> 20))
                        hb_trace_log("VSpWrite", written, length);
                    cursor.pos += length;
                }
                progressive.frames += count;
                video_received_group_frames = progressive.frames;
                progressive.partial = count < progressive.group_frames;
                __atomic_store_n(&link.written, cursor.pos, __ATOMIC_RELEASE);
                written = cursor.pos;
            } else {
                break;
            }
        }
    }
    if (video_spooling && link.video_flow_control && !closed)
        grant_video_credit();
    if (!progressive.ended && closed && !(h264_streaming && hb_h264_live_group_pending()) &&
        received - __atomic_load_n(&link.written, __ATOMIC_RELAXED) < 12) {
        failure = "Mac disconnected before finishing the video.";
        goto invalid;
    }
    if (!h264_streaming)
        return "Receiving H.264 video...";
    failure = "Hardware H.264 playback stopped.";
    if (progressive.ended && !progressive.partial &&
        !hb_h264_live_complete() && !hb_h264_live_finish())
        goto invalid;
    int result = hb_h264_live_tick();
    if (result < 0) {
        failure = hb_h264_live_error();
        goto invalid;
    }
    if (result > 0 && progressive.ended) {
        stop_h264_stream();
        set_state(DONE);
        return "Video finished.";
    }
    return h264_stream_status();

invalid:
    discard_video_spool();
    stop_h264_stream();
    return live_failure(failure);
}

static const char *spool_h264(uint32_t received)
{
    if (!video_spooling) {
        if (!hb_h264_can_receive())
            return live_failure("Close the current video first.");
        if (!(hb_fs_is_dir("/Data") || hb_fs_mkdir("/Data")) ||
            !hb_fs_stream_open(VIDEO_H264_RECEIVE_PATH))
            return live_failure("Could not open temporary video storage.");
        video_spooling = true;
        hb_wake_lock(true);
    }
    uint32_t written = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
    while (written < received) {
        uint32_t start = ring_offset(written);
        uint32_t count = received - written;
        if (count > RING_CAPACITY - start)
            count = RING_CAPACITY - start;
        if (!hb_fs_stream_write(link.bytes + start, count)) {
            discard_video_spool();
            return live_failure("Could not write the video. Check free space.");
        }
        written += count;
        __atomic_store_n(&link.written, written, __ATOMIC_RELEASE);
    }
    if (!live.verified || written != link.size) {
        lv_snprintf(status, sizeof(status), "Receiving H.264 video... %u%%",
                    (unsigned)((uint64_t)written * 100u / link.size));
        return status;
    }
    bool closed = hb_fs_stream_close();
    video_spooling = false;
    hb_wake_lock(false);
    if (!closed || hb_fs_size(VIDEO_H264_RECEIVE_PATH) != written) {
        hb_fs_remove(VIDEO_H264_RECEIVE_PATH);
        return live_failure("Could not finish saving the video.");
    }
    hb_os_free(link.bytes);
    link.bytes = NULL;
    const char *error = hb_h264_play_received(written);
    if (error)
        return live_failure(error);
    set_state(DONE);
    return "Video received.";
}
#endif

static const char *poll_live(unsigned current)
{
    if (current == CLOSED || current == FAILED) {
#ifdef NANO_YOUTUBE_AUDIO
        if (audio_report.stage && audio_report.stage < AUDIO_FINISHED)
            report_audio_progress(current == CLOSED ? AUDIO_DISCONNECTED : AUDIO_FAILED);
        if (link.audio_mode == AUDIO_RECEIVE_TEST && live.rate) {
            hb_wake_lock(false);
            live.rate = 0;
        }
#endif
#ifdef NANO_VIDEO_STREAM
        discard_video_spool();
        stop_h264_stream();
#endif
        if (live.playing)
            prepare_media_unload();
#ifndef NANO_VIDEO_STREAM
        close_audio_spool();
#endif
        return NULL;
    }
    if (current != RECEIVING && current != READY)
        return NULL;
#ifdef NANO_VIDEO_STREAM
    if (link.progressive)
        return poll_progressive(__atomic_load_n(&link.received, __ATOMIC_ACQUIRE), current == READY);
#endif
    if (current == READY && !live.verified) {
        if (link.hash != link.checksum) {
#ifdef NANO_VIDEO_STREAM
            discard_video_spool();
            stop_h264_stream();
#endif
            return live_failure("Stream checksum failed.");
        }
        live.verified = true;
    }
    uint32_t received = __atomic_load_n(&link.received, __ATOMIC_ACQUIRE);
#ifndef NANO_VIDEO_STREAM
#ifdef NANO_YOUTUBE_AUDIO
    if (link.audio_mode == AUDIO_RECEIVE_TEST) {
        if (!live.rate) {
            if (received < audio_header_size(received))
                return current == READY ? live_failure("Incomplete audio header.") : "Waiting for audio...";
            if (!parse_live_header())
                return live_failure("Unsupported audio format.");
            /* Keep the initial and terminal records on disk. During receipt
               this test uses the RAM trace and leaves the filesystem idle. */
            report_audio_progress(AUDIO_TEST_RECEIVING);
            hb_wake_lock(true);
        }
        __atomic_store_n(&link.written, received, __ATOMIC_RELEASE);
        if (current == READY && live.verified) {
            report_audio_progress(AUDIO_FINISHED);
            hb_wake_lock(false);
            set_state(DONE);
            return "Receive test passed.";
        }
        return "Receive test running...";
    }
    if (link.audio_mode == AUDIO_RECEIVE_SAVED)
#else
    size_t name_length = strlen(link.name);
    if (name_length >= 4 && !memcmp(link.name + name_length - 4, ".nsa", 4))
#endif
        return spool_audio(received);
#endif
#ifdef NANO_VIDEO_STREAM
    if (h264_streaming || (!live.rate && received >= 4 && !memcmp(link.bytes, "NVL1", 4)))
        return poll_h264_stream(received);
    if (video_spooling ||
        (!live.rate && received >= 4 &&
         (!memcmp(link.bytes, "NVH1", 4) || !memcmp(link.bytes, "NVH2", 4) ||
          !memcmp(link.bytes, "NVH3", 4) || !memcmp(link.bytes, "NVH4", 4) ||
          !memcmp(link.bytes, "NVH5", 4))))
        return spool_h264(received);
#endif
    if (!live.rate) {
#ifdef NANO_VIDEO_STREAM
        uint32_t header_size = MEDIA_HEADER_SIZE;
        if (received >= 4 && !memcmp(link.bytes, "NVS2", 4))
            header_size = VIDEO_LIVE_HEADER_BYTES;
#else
        uint32_t header_size = audio_header_size(received);
#ifdef NANO_YOUTUBE_AUDIO
        report_audio_progress(AUDIO_RECEIVING);
#endif
#endif
        if (received < header_size)
            return current == READY ? live_failure("Incomplete media header.") : "Waiting for media...";
        if (!parse_live_header())
            return live_failure("Unsupported media. Check the Mac sender.");
        __atomic_store_n(&link.written, header_size, __ATOMIC_RELEASE);
    }
    if (!live.playing) {
#ifdef NANO_VIDEO_STREAM
        uint32_t prebuffer = (live.side * live.side * live.fps + live.rate * live.channels * 2u) * 2u;
        if (current != READY && received - live.header_size < prebuffer)
            return "Buffering video...";
        if (!video_stream_start(live.side))
            return live_failure("Not enough free memory for the video stream.");
        live.startup_silence = live.output_rate * VIDEO_LEAD_MS / 1000u;
#else
        uint32_t header_size = live.audio_header;
        uint32_t prebuffer = live.rate * live.stride * 2u;
#ifdef NANO_YOUTUBE_AUDIO
        prebuffer = audio_wire_rate() * (live.mulaw || live.ima_channels ? 4u : 2u);
        /* Playback must start within the initial grant so it can free space
         * for the next grant, even at 44.1 kHz stereo. */
        const uint32_t initial_pcm = AUDIO_CREDIT_WINDOW - header_size;
        if (prebuffer > initial_pcm && !live.ima_channels) {
            prebuffer = initial_pcm - initial_pcm % live.stride;
        }
#endif
        if (current != READY && received - header_size < prebuffer) {
#ifdef NANO_YOUTUBE_AUDIO
            report_audio_progress(AUDIO_RECEIVING);
#endif
            return "Buffering audio...";
        }
        live.startup_silence = live.output_rate * 600u / 1000u;
#endif
#ifdef NANO_YOUTUBE_AUDIO
        report_audio_progress(AUDIO_STARTING_LIVE);
#endif
        if (!en_audio_start_live_stream(live.output_rate, pull_live, NULL)) {
#ifdef NANO_VIDEO_STREAM
            video_stream_reset();
#endif
            return live_failure("Could not start playback.");
        }
        live.playing = true;
        hb_trace_log("ALive", link.size, live.rate);
        hb_wake_lock(true);
    }
    uint32_t consumed = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
    uint32_t buffered = received >= consumed ? received - consumed : 0;
#ifdef NANO_VIDEO_STREAM
    uint32_t bytes_per_second = live.side * live.side * live.fps + live.rate * live.channels * 2u;
#elif defined(NANO_YOUTUBE_AUDIO)
    uint32_t bytes_per_second = audio_wire_rate();
#else
    uint32_t bytes_per_second = live.rate * live.stride;
#endif
    uint32_t refill = bytes_per_second * 3u;
    if (refill > RING_CAPACITY / 2u)
        refill = RING_CAPACITY / 2u;
    en_audio_tick_with_refill(live.verified || buffered >= refill);
#ifdef NANO_YOUTUBE_AUDIO
    report_audio_progress(AUDIO_PLAYING_LIVE);
#endif
#ifdef NANO_VIDEO_STREAM
    video_stream_present(en_audio_elapsed(), live.fps, live.side);
#endif
    if (en_audio_state() == EN_AUDIO_FAILED)
        return live_failure("Stream playback stopped.");
    if (en_audio_state() == EN_AUDIO_IDLE && live.verified) {
#ifdef NANO_YOUTUBE_AUDIO
        report_audio_progress(AUDIO_FINISHED);
#endif
        live.playing = false;
        hb_wake_lock(false);
#ifdef NANO_VIDEO_STREAM
        video_stream_reset();
#endif
        lv_snprintf(status, sizeof(status), "Stream finished.");
        set_state(DONE);
        return status;
    }
#ifdef NANO_VIDEO_STREAM
    return en_audio_stream_buffering() ? "Buffering video..." : "Playing video...";
#else
    if (en_audio_stream_buffering())
        return "Buffering audio...";
    unsigned channels = live.stride / 2u;
#ifdef NANO_YOUTUBE_AUDIO
    if (live.ima_channels)
        channels = live.ima_channels;
    else if (live.mulaw)
        channels = live.stride;
#endif
    lv_snprintf(status, sizeof(status), "Streaming %u Hz %s",
                (unsigned)live.rate, channels == 2 ? "stereo" : "mono");
    return status;
#endif
}
#elif !defined(HB_CLASSIC_RAW)
static bool remove_if_present(const char *path)
{
    return !hb_fs_exists(path) || hb_fs_remove(path);
}

static bool remove_pending_file(void)
{
    char name[CLASSIC_FILENAME_CAPACITY];
    if (classic_inbox_pending_name(name)) {
        char path[sizeof(disk.path)];
        lv_snprintf(path, sizeof(path), CLASSIC_INBOX "/%s", name);
        if (!remove_if_present(path))
            return false;
    }
    return remove_if_present(CLASSIC_PENDING);
}

static const char *open_receive_file(void)
{
    if ((!hb_fs_is_dir("/Data") && !hb_fs_mkdir("/Data")) ||
        (!hb_fs_is_dir(CLASSIC_INBOX) && !hb_fs_mkdir(CLASSIC_INBOX)))
        return "Could not create ClassicInbox.";
    if (!remove_pending_file())
        return "Could not clear an unfinished transfer.";
    char name[CLASSIC_FILENAME_CAPACITY];
    for (unsigned copy = 0; copy < 100; copy++) {
        if (copy)
            lv_snprintf(name, sizeof(name), "%u-%s", copy, link.name);
        else
            lv_snprintf(name, sizeof(name), "%s", link.name);
        lv_snprintf(disk.path, sizeof(disk.path), CLASSIC_INBOX "/%s", name);
        if (hb_fs_exists(disk.path))
            continue;
        if (!hb_fs_write(CLASSIC_PENDING, name, strlen(name))) {
            remove_if_present(CLASSIC_PENDING);
            return "Could not write the transfer marker.";
        }
        if (!hb_fs_stream_open(disk.path)) {
            remove_pending_file();
            return "Could not open the WAV file for writing.";
        }
        disk.open = true;
        disk.hash = 2166136261u;
        return NULL;
    }
    return "Too many saved copies of this file.";
}

static void discard_receive_file(void)
{
    if (disk.open) {
        hb_fs_stream_close();
        disk.open = false;
        remove_pending_file();
    }
}

static const char *receive_failed(const char *message)
{
    uintptr_t guard = 0;
    lock(&guard);
    link.failure = message;
    set_state(FAILED);
    unlock(&guard);
    discard_receive_file();
    return message;
}

static const char *flush_receive_file(void)
{
    if (!disk.open) {
        const char *error = open_receive_file();
        if (error)
            return receive_failed(error);
    }

    uint32_t written = __atomic_load_n(&link.written, __ATOMIC_RELAXED);
    uint32_t received = __atomic_load_n(&link.received, __ATOMIC_ACQUIRE);
    uint32_t start = written % RECEIVE_CAPACITY;
    uint32_t count = received - written;
    if (count > RECEIVE_CAPACITY - start)
        count = RECEIVE_CAPACITY - start;
    if (count > DISK_WRITE_SIZE)
        count = DISK_WRITE_SIZE;
    if (count && (received - written >= 4096u || state() == READY)) {
        const uint8_t *data = link.bytes + start;
        if (!hb_fs_stream_write(data, count))
            return receive_failed("Could not write the audio file.");
        for (uint32_t i = 0; i < count; i++)
            disk.hash = (disk.hash ^ data[i]) * 16777619u;
        written += count;
        __atomic_store_n(&link.written, written, __ATOMIC_RELEASE);
    }

    if (state() == READY && written == link.size) {
        if (disk.hash != link.checksum)
            return receive_failed("Classic file checksum failed.");
        bool closed_ok = hb_fs_stream_close();
        disk.open = false;
        if (!closed_ok || hb_fs_size(disk.path) != link.size || !hb_fs_remove(CLASSIC_PENDING)) {
            remove_pending_file();
            return receive_failed("Could not finish saving the file.");
        }
        lv_snprintf(status, sizeof(status), "File saved. Open Audio Player.");
        set_state(DONE);
        return status;
    }

    lv_snprintf(status, sizeof(status), "Receiving audio... %u%%",
                link.size ? (unsigned)((uint64_t)written * 100u / link.size) : 0);
    return status;
}
#endif

static bool transfer_active(void)
{
#ifdef NANO_VIDEO_STREAM
    if (video_spooling || h264_streaming)
        return true;
#endif
#ifdef NANO_STREAM
#ifndef NANO_VIDEO_STREAM
    if (live.spooling || live.reading)
        return true;
#endif
    return live.playing || state() == READY;
#elif defined(HB_CLASSIC_RAW)
    return false;
#else
    return disk.open || state() == READY;
#endif
}

static unsigned listen_classic(const uint8_t address[6], uint16_t psm
#ifdef HB_CLASSIC_RAW
                               , hb_classic_receive_fn receive, void *context
#endif
                               )
{
    if (*(const volatile uint32_t *)0x080f7764u != 0x41f0e92du ||
        *(const volatile uint32_t *)0x080f7580u != 0x41f0e92du ||
        *(const volatile uint32_t *)0x083d0fb4u != 0x4ff0e92du ||
        *(const volatile uint16_t *)0x08410eb8u != 0xb538u ||
        *(const volatile uint32_t *)0x080f90ccu != 0x0877eb70u)
        return 1;
    uintptr_t guard = 0;
    lock(&guard);
    if (link.cid || link.listening || transfer_active()) {
        unlock(&guard);
        return 2;
    }
    memset(link.name, 0, sizeof(link.name));
    link.size = link.received = link.written = 0;
    link.open_error = link.close_reason = 0;
    link.failure = NULL;
#ifdef NANO_YOUTUBE_AUDIO
    link.header_bytes = 0;
#endif
#ifdef NANO_VIDEO_STREAM
    link.progressive = link.video_flow_control = false;
    link.header_bytes = 0;
    memset(&progressive, 0, sizeof(progressive));
#endif
#ifdef NANO_STREAM
    live.rate = live.repeat = 0;
    live.verified = false;
#endif
    link.psm = psm;
#ifdef HB_CLASSIC_RAW
    link.receive = receive;
    link.receive_context = context;
#endif
    memcpy(link.peer, address, sizeof(link.peer));
    __atomic_store_n(&link.confirmed, false, __ATOMIC_RELAXED);
    __atomic_store_n(&local_pair.code, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&local_pair.result, 0, __ATOMIC_RELAXED);
    pair.start_error = 0;
    pair.replied = false;
    link.stopping = false;
    link.retired_ms = 0;
    unsigned error = ((listen_fn)0x080f7765u)(psm, incoming);
    link.listening = error == 0;
    link.callbacks_installed |= link.listening;
    set_state(error ? FAILED : LISTENING);
    unlock(&guard);
    return error;
}

#ifdef HB_CLASSIC_RAW
unsigned hb_classic_listen(uint16_t psm, const uint8_t peer[6],
                           hb_classic_receive_fn receive, void *context)
{
    if (!peer || !receive || psm < 0x1001u || (psm & 0x0101u) != 1u)
        return 1;
    return listen_classic(peer, psm, receive, context);
}

bool hb_classic_connected(void) { return state() == WAITING && link.cid != 0; }

unsigned hb_classic_outgoing_mtu(void)
{
    return hb_classic_connected() ? link.out_mtu : 0;
}

unsigned hb_classic_send(const void *bytes, unsigned length)
{
    if (!bytes || !length || length > UINT16_MAX || !sdu_send_supported())
        return 1;
    uintptr_t guard = 0;
    lock(&guard);
    unsigned result = 2;
    if (link.cid && state() == WAITING && length <= link.out_mtu)
        result = send_sdu(bytes, length);
    unlock(&guard);
    return result;
}
#else
unsigned hb_classic_listen_media(const uint8_t address[6])
{
    return address ? listen_classic(address, CLASSIC_PSM) : 1;
}
#endif

static void stop_pairing(void)
{
    struct pair_mailbox *events = pair.events;
    uintptr_t agent = __atomic_exchange_n(&events->agent, 0, __ATOMIC_ACQ_REL);
    if (agent && events == &local_pair) {
        ((unsigned (*)(uintptr_t))PAIR_STOP)(agent);
        ((void (*)(uintptr_t *))PAIR_DESTROY)(&agent);
    }
    __atomic_store_n(&events->code, 0, __ATOMIC_RELEASE);
    pair.events = &local_pair;
}

void hb_classic_disconnect(void)
{
    stop_pairing();
    uintptr_t guard = 0;
    lock(&guard);
    link.stopping = true;
    set_state(CLOSED);
    if (link.cid)
        ((unsigned (*)(unsigned))0x08410eb9u)(link.cid);
    if (link.listening && !link.cid &&
        !((unsigned (*)(unsigned))0x080f7581u)(link.psm)) {
        link.listening = false;
        set_state(CLOSED);
    }
    unlock(&guard);
#ifdef NANO_VIDEO_STREAM
    if (video_prep_awake) {
        video_prep_awake = false;
        hb_wake_lock(false);
    }
    discard_video_spool();
    stop_h264_stream();
#elif !defined(NANO_STREAM) && !defined(HB_CLASSIC_RAW)
    discard_receive_file();
#endif
}

bool hb_classic_busy(void)
{
    uintptr_t guard = 0;
    lock(&guard);
    if (link.cid) {
        uintptr_t native_channel = 0;
        if (((unsigned (*)(unsigned, uintptr_t *))0x0841afe5u)(link.cid, &native_channel)) {
            link.cid = 0;
            unsigned current = state();
            if (current == OPENING) {
                link.failure = "Classic channel disappeared while opening.";
                set_state(FAILED);
            } else if (current == RECEIVING || current == WAITING) {
#ifdef NANO_VIDEO_STREAM
                if (current == RECEIVING && link.progressive)
                    set_state(READY);
                else
#endif
                    set_state(CLOSED);
            }
        }
    }
    if (link.listening && !link.cid && (link.stopping || state() != LISTENING) &&
        !((unsigned (*)(unsigned))0x080f7581u)(link.psm))
        link.listening = false;
    bool busy = link.cid != 0 || link.listening || transfer_active();
#ifdef NANO_VIDEO_STREAM
    uint8_t *buffer = busy ? NULL : link.bytes;
    if (!busy)
        link.bytes = NULL;
#endif
#ifdef NANO_YOUTUBE_AUDIO
    uint8_t *artwork = busy ? NULL : link.artwork;
    if (!busy)
        link.artwork = NULL;
#endif
    unlock(&guard);
#ifdef NANO_YOUTUBE_AUDIO
    hb_os_free(artwork);
#endif
#ifdef NANO_VIDEO_STREAM
    hb_os_free(buffer);
#endif
    if (!busy)
        stop_pairing();
    if (!busy && link.callbacks_installed && !link.retired_ms)
        link.retired_ms = hb_time_uptime_ms();
    return busy;
}

int hb_classic_prepare_unload(void)
{
#ifdef NANO_STREAM
    if (!prepare_media_unload())
        return 0;
#endif
    if (!link.callbacks_installed)
        return 1;
    if (hb_classic_busy()) {
        hb_classic_disconnect();
        link.retired_ms = 0;
        return 0;
    }
    uint32_t now = hb_time_uptime_ms();
    if (!link.retired_ms) {
        link.retired_ms = now;
        return 0;
    }
    if (now - link.retired_ms < 100u)
        return 0;
    link.callbacks_installed = false;
    return 1;
}

const char *hb_classic_poll(void)
{
    unsigned current = state();
#ifdef NANO_VIDEO_STREAM
    const char *preparing = NULL;
    uintptr_t prep_guard = 0;
    lock(&prep_guard);
    if (state() == WAITING && link.video_prep_stage &&
        hb_time_uptime_ms() - link.video_prep_ms < 90000u) {
        static const char *const stages[] = {"", "Downloading video from YouTube...", "Preparing H.264 on the Mac...",
                                "Starting video transfer..."};
        preparing = stages[link.video_prep_stage];
    } else {
        link.video_prep_stage = 0;
    }
    unlock(&prep_guard);
    if (video_prep_awake != (preparing != NULL)) {
        video_prep_awake = preparing != NULL;
        hb_wake_lock(video_prep_awake);
    }
    if (current == WAITING && link.native_video_announcements < VIDEO_CAPABILITY_REPEAT_COUNT && sdu_send_supported()) {
        uintptr_t guard = 0;
        lock(&guard);
        uint32_t now = hb_time_uptime_ms();
        if (link.cid && state() == WAITING &&
            (!link.native_video_hello_ms || now - link.native_video_hello_ms >= 500u))
            announce_native_video();
        unlock(&guard);
    }
    if (preparing)
        return preparing;
#endif
#ifdef NANO_YOUTUBE_AUDIO
    if (current == RECEIVING) {
        uint32_t now = hb_time_uptime_ms();
        if (now - live.trace_ms >= AUDIO_TRACE_INTERVAL_MS) {
            live.trace_ms = now;
            hb_trace_log(live.spooling ? "ASavePos" : live.playing ? "ALivePos" : "ARecvPos",
                         __atomic_load_n(&link.received, __ATOMIC_ACQUIRE),
                         __atomic_load_n(&link.written, __ATOMIC_RELAXED));
        }
        if (link.mac_paced_audio)
            update_audio_sender_pause();
        else
            grant_audio_credit();
    }
#endif
#ifdef NANO_STREAM
    const char *live_status = poll_live(current);
    if (live_status)
        return live_status;
#endif
    if (current == LISTENING) {
        unsigned result = __atomic_load_n(&pair.events->result, __ATOMIC_ACQUIRE);
        if (result == 1)
            return "Classic paired. Click Connect on the Mac.";
        if (result) {
            lv_snprintf(status, sizeof(status), "Pairing ended (%u). Try Pair again on the Mac.", result - 1u);
            return status;
        }
        if (pair.replied)
            return "Confirm the matching code on the Mac.";
        if (pair.attach_failure)
            return pair.attach_failure;
        if (pair.start_error) {
            lv_snprintf(status, sizeof(status), "Classic listening. Pairing unavailable (0x%02X).",
                        pair.start_error);
            return status;
        }
        if (pair.events != &local_pair)
            return "System pairing ready. Click Pair or Connect on the Mac.";
        return "Classic ready. Click Pair or Connect on the Mac.";
    }
    if (current == OPENING)
        return "Opening Classic L2CAP...";
#ifdef NANO_YOUTUBE_SEARCH
    if (current == WAITING || current == DONE) {
        uintptr_t guard = 0;
        lock(&guard);
        uint32_t now = hb_time_uptime_ms();
#ifdef NANO_YOUTUBE_AUDIO
        static const uint8_t hello[][5] = {"NYL4", "NYL5", "NYH1", "NYL5", "NYL4"};
#else
        static const uint8_t hello[][5] = {"NYH1", "NYH1", "NYH1"};
#endif
        if (link.cid && link.artwork_announcements < sizeof(hello) / sizeof(hello[0]) &&
            (!link.artwork_hello_ms || now - link.artwork_hello_ms >= 1000u) && sdu_send_supported()) {
            link.artwork_hello_ms = now;
            if (!send_sdu(hello[link.artwork_announcements], sizeof(hello[0]) - 1u))
                link.artwork_announcements++;
        }
#ifdef NANO_YOUTUBE_VIDEO
        poll_video_play_request(now);
        if (link.cid && current == DONE && !link.video_ready_announced && hb_h264_can_receive() &&
            (!link.artwork_hello_ms || now - link.artwork_hello_ms >= 1000u) && sdu_send_supported()) {
            const uint8_t ready[] = {'N', 'Y', 'V', '1'};
            link.artwork_hello_ms = now;
            link.video_ready_announced = send_sdu(ready, sizeof(ready)) == 0;
        }
#endif
        if (__atomic_load_n(&link.search_pending, __ATOMIC_ACQUIRE) &&
            now - link.search_started_ms >= 90000u) {
            memcpy(link.search_status, "Mac did not reply. Reconnect and try again.",
                   sizeof("Mac did not reply. Reconnect and try again."));
            __atomic_store_n(&link.search_pending, false, __ATOMIC_RELEASE);
        }
        if (current == DONE)
            link.search_status[0] = 0;
        memcpy(status, link.search_status, sizeof(link.search_status));
        bool have_songs = link.songs.count != 0;
        unlock(&guard);
#ifdef NANO_YOUTUBE_AUDIO
        if (current == DONE && link.audio_mode == AUDIO_RECEIVE_TEST)
            return "Receive test passed.";
#endif
#ifdef NANO_YOUTUBE_VIDEO
        if (current == DONE && !hb_h264_can_receive())
            return "Close the video to choose another.";
#endif
        return status[0] ? status : have_songs ? CHOOSE_STATUS : SEARCH_STATUS;
    }
#else
    if (current == WAITING)
#ifdef HB_CLASSIC_RAW
        return "Classic L2CAP connected.";
#elif defined(NANO_VIDEO_STREAM)
        return "Connected. Send an MP4 from the Mac.";
#else
        return "Classic connected. Send audio from the Mac.";
#endif
#endif
#if !defined(NANO_STREAM) && !defined(HB_CLASSIC_RAW)
    if (current == RECEIVING || current == READY)
        return flush_receive_file();
#endif
    if (current == FAILED) {
#if !defined(NANO_STREAM) && !defined(HB_CLASSIC_RAW)
        discard_receive_file();
#endif
        if (link.open_error) {
            lv_snprintf(status, sizeof(status), "Classic L2CAP rejected.\nFirmware error 0x%04X (%u).",
                        link.open_error, link.open_error);
            return status;
        }
        return link.failure ? link.failure : "Classic L2CAP open failed.";
    }
    if (current == CLOSED) {
#if !defined(NANO_STREAM) && !defined(HB_CLASSIC_RAW)
        discard_receive_file();
#endif
        if (link.close_reason) {
            lv_snprintf(status, sizeof(status), "Classic link closed.\nReason 0x%04X (%u).",
                        link.close_reason, link.close_reason);
            return status;
        }
        return "Classic disconnected.";
    }
    return status;
}
