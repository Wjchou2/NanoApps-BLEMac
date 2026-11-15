/* Classic inquiry and BLE diagnostics on firmware 39A10023. */
#include "hb_sdk.h"
#include "hb_shared.h"
#include "hb_heap.h"
#ifdef NANO_YOUTUBE_SEARCH
#include "hb_lv_surface.h"
#endif
#ifndef NANO_STREAM
#include "hb_pcm.h"
#include "hb_classic_inbox.h"
#endif

#include "hb_classic.h"
#include "hb_ble.h"
#include "hb_bt_internal.h"
#include "lvgl/lvgl.h"
#include <string.h>
#ifdef NANO_VIDEO_STREAM
#include "hb_h264.h"
#include "hb_video_surface.h"
#endif

/* The nano 7 has 64 MiB of SDRAM starting at 0x08000000. */
#define BT_AGENT_GET_DEVICES 0x080cd1e5u
#define BT_DEVICE_GET_ADDRESS_TEXT 0x080ccdb5u
#define BT_DEVICE_GET_NAME 0x080ccf5du
#define BT_GET_MODULE_POWER 0x08417069u
#define BT_GET_TEST_MODE 0x080cd649u
/* The discovery agent accepts every Classic inquiry result for this mask. */
#define BT_ANY_SERVICE UINT32_MAX
#define SCAN_POLL_MS 250u
#define SCAN_WAIT_MS 30000u

#define DEVICE_LIMIT 24u
#define CLASSIC_FOUND_VTABLE 0x087ec52cu
#define CLASSIC_FOUND_WORKER 0x08435569u

#ifndef NANO_STREAM
extern const uint8_t classic_capture_start[], classic_capture_mailbox[], classic_capture_end[];
static struct {
    uint32_t enabled, count;
    uint8_t address[DEVICE_LIMIT][8];
} *classic_capture;
static unsigned classic_raw_shown;
#endif

static int prepare_unload(void);
static void close_classic_pair_prompt(void);
#ifndef NANO_STREAM
static void close_classic_inbox(lv_event_t *event);
#endif
static void close_address_dialog(lv_event_t *event);
static void stop_polling(void);
static void begin_classic_link(void);
static bool mac_beacon_address(const char *name, uint8_t address[6]);

typedef void (*firmware_lock_fn)(uintptr_t *guard, void *mutex);
typedef void (*firmware_unlock_fn)(uintptr_t *guard);
typedef int (*get_module_power_fn)(uintptr_t device, uint32_t module, uint32_t *on);
typedef int (*get_test_mode_fn)(uintptr_t device, uint32_t *enabled);
typedef int (*start_scan_fn)(uintptr_t client, uint32_t services);
typedef int (*get_devices_fn)(uintptr_t agent, uintptr_t *out,
                              uint32_t *count, uint32_t capacity);
typedef int (*get_device_text_fn)(uintptr_t device, char *out, uint32_t capacity);
static lv_obj_t *status_label;
static lv_obj_t *device_list;
#ifndef NANO_STREAM
static lv_obj_t *classic_button;
#endif
static lv_obj_t *ble_button;
static lv_obj_t *address_button;
static lv_obj_t *connect_button;
static lv_obj_t *connect_label;
static lv_obj_t *selected_row;
#ifndef NANO_STREAM
static const hb_ble_device_t *selected_device;
static uint8_t classic_addresses[DEVICE_LIMIT][6];
#endif

static const uint8_t *selected_classic;
#define CLASSIC_PEER_PATH "/Data/ClassicSenderPeer.bin"
static uint8_t remembered_classic[6], beacon_classic[6];
static bool classic_remembered, classic_peer_saved;
static struct {
    lv_obj_t *dialog, *text, *listen;
    unsigned digits;
    char hex[12];
    uint8_t address[6];
} classic_entry;
static bool classic_closing;
static lv_obj_t *classic_pair_prompt;
#ifndef NANO_STREAM
static lv_obj_t *classic_inbox, *classic_inbox_status;
#endif

static unsigned classic_pair_value;
static lv_timer_t *scan_timer;
#ifdef NANO_STREAM
static lv_timer_t *startup_timer;
#endif
static uintptr_t scan_client;
#ifndef NANO_STREAM
static uint32_t scan_started_ms;
static bool nearby_query_succeeded;
static struct {
    uintptr_t device;
    lv_obj_t *row;
    bool named;
} nearby_devices[DEVICE_LIMIT];
static unsigned nearby_count;
#endif


#ifndef NANO_STREAM
static bool arm_classic_capture(void)
{
    if (!hb_shared || read_pointer(0x0843553cu) != CLASSIC_FOUND_VTABLE ||
        read_pointer(0x0843559cu) != 0xf926f682u)
        return false;

    size_t code_size = (uintptr_t)classic_capture_mailbox - (uintptr_t)classic_capture_start;
    size_t size = (uintptr_t)classic_capture_end - (uintptr_t)classic_capture_start;
    for (unsigned i = 0; i < 4; i++) {
        uintptr_t slot = CLASSIC_FOUND_VTABLE + i * sizeof(uintptr_t);
        uintptr_t worker = read_pointer(slot);
        uintptr_t code = worker & ~(uintptr_t)1u;
        if (worker == CLASSIC_FOUND_WORKER) {
            void *copy = hb_os_alloc(size);
            if (!copy)
                return false;
            memcpy(copy, classic_capture_start, size);
            code = (uintptr_t)copy;
            hb_icache_invalidate(code, size);
            __atomic_store_n((uintptr_t *)slot, code | 1u, __ATOMIC_RELEASE);
        } else if (!(worker & 1u) || code < 0x08900000u || !in_sdram(code, size) ||
                   memcmp((const void *)code, classic_capture_start, code_size)) {
            continue;
        }
        /* Retain one small OS-heap thunk until reboot. It contains no app
         * pointers and forwards stock events even after the app unloads. */
        classic_capture = (void *)(code + code_size);
        classic_raw_shown = 0;
        __atomic_store_n(&classic_capture->count, 0, __ATOMIC_RELAXED);
        __atomic_store_n(&classic_capture->enabled, 1, __ATOMIC_RELEASE);
        hb_shared->prepare_unload = prepare_unload;
        return true;
    }
    return false;
}

static void disarm_classic_capture(void)
{
    if (classic_capture)
        __atomic_store_n(&classic_capture->enabled, 0, __ATOMIC_RELEASE);
    classic_capture = NULL;
}
#endif

#ifdef NANO_STREAM
#define LISTEN_LABEL "Listen"
#else
#define LISTEN_LABEL "Listen Classic"
#endif

static void select_classic_address(const uint8_t address[6])
{
    if (scan_client)
        stop_polling();
    if (selected_row) {
        lv_obj_remove_state(selected_row, LV_STATE_CHECKED);
        selected_row = NULL;
    }
    selected_classic = address;
#ifndef NANO_STREAM
    selected_device = NULL;
#endif
    lv_label_set_text(connect_label, LISTEN_LABEL);
    lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
#ifdef NANO_STREAM
    lv_label_set_text(status_label, "Tap Listen, then click Connect in Nano Bluetooth Link.");
#else
    lv_label_set_text(status_label, "Tap Listen Classic, then connect from the Mac.");
#endif
}

static void select_classic(lv_event_t *event)
{
    if (!scan_timer || scan_client)
        select_classic_address(lv_event_get_user_data(event));
}

static bool valid_classic_address(const uint8_t address[6])
{
    static const uint8_t empty[6], broadcast[6] = {255, 255, 255, 255, 255, 255};
    return memcmp(address, empty, sizeof(empty)) && memcmp(address, broadcast, sizeof(broadcast));
}

static void add_remembered_row(void)
{
    if (!classic_remembered)
        return;
    const uint8_t *a = remembered_classic;
    char label[32];
    lv_snprintf(label, sizeof(label), "Saved Mac\n%02X:%02X:%02X:%02X:%02X:%02X",
                a[0], a[1], a[2], a[3], a[4], a[5]);
    lv_obj_t *row = lv_list_add_button(device_list, NULL, label);
    lv_obj_add_event_cb(row, select_classic, LV_EVENT_CLICKED, remembered_classic);
#ifdef NANO_STREAM
    lv_obj_remove_flag(device_list, LV_OBJ_FLAG_HIDDEN);
#endif
}

#ifndef NANO_STREAM
static void show_classic_capture(void)
{
    if (!classic_capture)
        return;
    unsigned count = __atomic_load_n(&classic_capture->count, __ATOMIC_ACQUIRE);
    while (classic_raw_shown < count && classic_raw_shown < DEVICE_LIMIT) {
        uint8_t *address = classic_addresses[classic_raw_shown];
        memcpy(address, classic_capture->address[classic_raw_shown++], 6);
        char label[40];
        lv_snprintf(label, sizeof(label), "Raw %02X:%02X:%02X:%02X:%02X:%02X",
                    address[0], address[1], address[2], address[3], address[4], address[5]);
        lv_obj_t *row = lv_list_add_button(device_list, NULL, label);
        lv_obj_add_event_cb(row, select_classic, LV_EVENT_CLICKED, address);
    }
}
#endif

static uintptr_t get_model(void)
{
    if (!firmware_matches()) {
        lv_label_set_text(status_label, "Firmware signature mismatch.");
        return 0;
    }

    uintptr_t model = read_pointer(BT_MODEL_SLOT);
    if (!model) {
        lv_label_set_text(status_label, "Open Settings > Bluetooth first.");
        return 0;
    }
    if (!in_sdram(model, BT_CLIENT_OFFSET + sizeof(uintptr_t)) ||
        read_pointer(model) != BT_MODEL_VTABLE) {
        lv_label_set_text(status_label, "Bluetooth list is unavailable.");
        return 0;
    }
    return model;
}

static const char *client_error(uintptr_t client)
{
    if (!client)
        return "Settings has no Bluetooth client.";
    if (!in_sdram(client, BT_CLIENT_PAIR_AGENT + sizeof(uintptr_t)))
        return "Bluetooth client address is outside RAM.";
    if (read_pointer(client) != BT_CLIENT_VTABLE)
        return "Bluetooth client table mismatch.";
    if (read_pointer(client + BT_CLIENT_TYPE) != BT_CLASSIC_MODULE)
        return "Bluetooth client type mismatch.";
    if (read_pointer(BT_CLIENT_VTABLE + BT_CLIENT_START_SLOT) != BT_CLIENT_START)
        return "Bluetooth scan method mismatch.";
    if (!read_pointer(client + BT_CLIENT_SESSION))
        return "Bluetooth session is closed. Open Bluetooth Settings.";
    return NULL;
}

static void stop_polling(void)
{
#ifndef NANO_STREAM
    disarm_classic_capture();
#endif
    lv_timer_delete(scan_timer);
    scan_timer = NULL;
    scan_client = 0;
#ifndef NANO_STREAM
    lv_obj_remove_state(classic_button, LV_STATE_DISABLED);
#else
    lv_obj_remove_flag(ble_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(address_button, LV_OBJ_FLAG_HIDDEN);
    if (lv_obj_get_child_count(device_list))
        lv_obj_remove_flag(device_list, LV_OBJ_FLAG_HIDDEN);
#endif
    lv_obj_remove_state(ble_button, LV_STATE_DISABLED);
    lv_obj_remove_state(address_button, LV_STATE_DISABLED);
#ifdef NANO_STREAM
    if (selected_classic)
#else
    if (selected_device || selected_classic)
#endif
        lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
}

static void clear_results(void)
{
    selected_row = NULL;
#ifndef NANO_STREAM
    selected_device = NULL;
#endif
    selected_classic = NULL;
    lv_label_set_text(connect_label, "Connect");
    lv_obj_add_state(connect_button, LV_STATE_DISABLED);
    lv_obj_clean(device_list);
#ifdef NANO_STREAM
    lv_obj_add_flag(device_list, LV_OBJ_FLAG_HIDDEN);
#endif
    add_remembered_row();
}

#ifndef NANO_STREAM
static void add_nearby_device(uintptr_t device)
{
    if (!device)
        return;
    unsigned slot = 0;
    while (slot < nearby_count && nearby_devices[slot].device != device)
        slot++;
    if (slot == DEVICE_LIMIT || (slot < nearby_count && nearby_devices[slot].named))
        return;

    char name[256] = { 0 };
    get_device_text_fn get_name = (get_device_text_fn)BT_DEVICE_GET_NAME;
    if (get_name(device, name, sizeof(name)))
        name[0] = 0;
    name[sizeof(name) - 1] = 0;
    bool has_name = false;
    for (char *p = name; *p; p++) {
        if ((unsigned char)*p < 32 || *p == 127)
            *p = ' ';
        has_name |= *p != ' ';
    }
    if (slot < nearby_count && !has_name)
        return;

    char address[18] = {0};
    get_device_text_fn get_address = (get_device_text_fn)BT_DEVICE_GET_ADDRESS_TEXT;
    if (get_address(device, address, sizeof(address)))
        address[0] = 0;
    address[sizeof(address) - 1] = 0;

    char label[sizeof(name) + sizeof(address)];
    lv_snprintf(label, sizeof(label), "%s%s%s", has_name ? name : "Unnamed Classic device",
                address[0] ? "\n" : "", address);
    if (slot == nearby_count) {
        nearby_devices[slot].device = device;
        nearby_devices[slot].row = lv_list_add_text(device_list, label);
        nearby_count++;
    } else {
        lv_label_set_text(nearby_devices[slot].row, label);
    }
    nearby_devices[slot].named = has_name;
}

static bool poll_nearby_devices(uintptr_t client)
{
    firmware_lock_fn lock_client = (firmware_lock_fn)BT_CLIENT_LOCK;
    firmware_unlock_fn unlock_client = (firmware_unlock_fn)BT_CLIENT_UNLOCK;
    get_devices_fn get_devices = (get_devices_fn)BT_AGENT_GET_DEVICES;
    uintptr_t guard = 0;
    uintptr_t devices[DEVICE_LIMIT];
    uint32_t count = 0;

    /* The stock completion callback takes this lock before freeing the agent.
     * Keep it held while obtaining the device handles, then resolve names. */
    lock_client(&guard, (void *)(client + BT_CLIENT_MUTEX));
    uintptr_t agent = read_pointer(client + BT_CLIENT_AGENT);
    int error = agent ? get_devices(agent, devices, &count, DEVICE_LIMIT) : 0;
    unlock_client(&guard);

    if (!agent)
        return false;
    if (error || count > DEVICE_LIMIT)
        return true;

    nearby_query_succeeded = true;
    unsigned previous_count = nearby_count;
    for (unsigned i = 0; i < count; i++)
        add_nearby_device(devices[i]);
    if (nearby_count != previous_count)
        lv_label_set_text_fmt(status_label, "Scanning... %u Classic devices", nearby_count);
    return true;
}

static void poll_scan(lv_timer_t *timer)
{
    (void)timer;
    uintptr_t model = get_model();
    if (!model) {
        stop_polling();
        return;
    }
    if (read_pointer(model + BT_CLIENT_OFFSET) != scan_client) {
        stop_polling();
        lv_label_set_text(status_label, "Bluetooth client changed.");
        return;
    }
    const char *error = client_error(scan_client);
    if (error) {
        stop_polling();
        lv_label_set_text(status_label, error);
        return;
    }

    bool running = poll_nearby_devices(scan_client);
    show_classic_capture();
    if (!running) {
        bool captured = classic_capture != NULL;
        stop_polling();
        if (captured) {
            lv_label_set_text_fmt(status_label, "%u raw / %u listed Classic devices",
                                  classic_raw_shown, nearby_count);
        } else if (nearby_count) {
            lv_label_set_text_fmt(status_label, "Raw capture unavailable.\n%u listed Classic devices",
                                  nearby_count);
        } else {
            lv_label_set_text(status_label, nearby_query_succeeded
                                             ? "Raw capture unavailable.\nNo Classic devices returned."
                                             : "Raw capture unavailable.\nCould not read scan results.");
        }
    } else if (hb_time_uptime_ms() - scan_started_ms >= SCAN_WAIT_MS) {
        stop_polling();
        lv_label_set_text(status_label, "Scan still busy. Select a result or enter the Mac address.");
    }
}

static void begin_scan(lv_event_t *event)
{
    (void)event;
    if (scan_timer)
        return;

    uintptr_t model = get_model();
    if (!model)
        return;

    uintptr_t client = read_pointer(model + BT_CLIENT_OFFSET);
    const char *error = client_error(client);
    if (error) {
        lv_label_set_text(status_label, error);
        return;
    }

    uintptr_t device = read_pointer(client + BT_CLIENT_LOCAL_DEVICE);
    get_module_power_fn get_power = (get_module_power_fn)BT_GET_MODULE_POWER;
    uint32_t powered = 0;
    if (!device || get_power(device, BT_CLASSIC_MODULE, &powered)) {
        lv_label_set_text(status_label, "Could not read Bluetooth state.");
        return;
    }
    if (powered != UINT32_MAX) {
        lv_label_set_text(status_label, "Turn Bluetooth on in Settings.");
        return;
    }
    get_test_mode_fn get_test_mode = (get_test_mode_fn)BT_GET_TEST_MODE;
    uint32_t test_mode = 0;
    if (get_test_mode(device, &test_mode) || test_mode) {
        lv_label_set_text(status_label, "Bluetooth discovery is unavailable.");
        return;
    }

    scan_timer = lv_timer_create(poll_scan, SCAN_POLL_MS, NULL);
    if (!scan_timer) {
        lv_label_set_text(status_label, "Could not start scan.");
        return;
    }
    scan_client = client;
    scan_started_ms = hb_time_uptime_ms();
    nearby_query_succeeded = false;
    nearby_count = 0;
    lv_obj_add_state(classic_button, LV_STATE_DISABLED);
    lv_obj_add_state(ble_button, LV_STATE_DISABLED);
    clear_results();
    lv_label_set_text(status_label, arm_classic_capture()
                      ? "Scanning for raw Classic results..."
                      : "Raw capture unavailable. Scanning normally...");

    /* The stock client owns the agent and its callbacks through completion.
     * LVGL only polls it, so leaving this screen requires no app callback
     * to remain registered with the Bluetooth stack. */
    bool resumed = read_pointer(client + BT_CLIENT_AGENT) != 0;
    if (!resumed && ((start_scan_fn)BT_CLIENT_START)(client, BT_ANY_SERVICE))
        return;
    if (resumed || read_pointer(client + BT_CLIENT_AGENT)) {
        lv_label_set_text(status_label, "Reading the current Classic scan...");
        return;
    }
    stop_polling();
    lv_label_set_text(status_label, "Could not start scan. Enter the Mac address.");
}

#endif

static unsigned hex_value(char digit)
{
    return digit <= '9' ? digit - '0' : digit - 'A' + 10;
}

/* The sender advertises its Classic address as NM- plus 12 hex digits. */
static bool mac_beacon_address(const char *name, uint8_t address[6])
{
    if (strlen(name) != 15 || memcmp(name, "NM-", 3))
        return false;
    for (unsigned i = 3; i < 15; i++)
        if (!((name[i] >= '0' && name[i] <= '9') || (name[i] >= 'A' && name[i] <= 'F')))
            return false;
    for (unsigned i = 0; i < 6; i++)
        address[i] = (uint8_t)(hex_value(name[3 + 2 * i]) << 4 | hex_value(name[4 + 2 * i]));
    return valid_classic_address(address);
}

static void select_ble_device(lv_event_t *event)
{
    if (scan_timer)
        return;

    const hb_ble_device_t *device = lv_event_get_user_data(event);
    uint8_t address[6];
    if (mac_beacon_address(device->name, address)) {
        memcpy(beacon_classic, address, sizeof(beacon_classic));
        select_classic_address(beacon_classic);
        selected_row = lv_event_get_target_obj(event);
        lv_obj_add_state(selected_row, LV_STATE_CHECKED);
#ifdef NANO_STREAM
        begin_classic_link();
#endif
        return;
    }

#ifndef NANO_STREAM
    if (selected_row)
        lv_obj_remove_state(selected_row, LV_STATE_CHECKED);
    selected_row = lv_event_get_target_obj(event);
    selected_device = device;
    selected_classic = NULL;
    lv_obj_add_state(selected_row, LV_STATE_CHECKED);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
    lv_label_set_text_fmt(status_label, "Selected %s", selected_device->name);
#endif
}

#ifdef NANO_STREAM
static bool sender_beacon(const hb_ble_device_t *device)
{
    uint8_t mac[6];
    return mac_beacon_address(device->name, mac);
}
#endif

static void scan_ble(void)
{
    static hb_ble_device_t found[HB_BLE_SCAN_LIMIT];
    unsigned count;
#ifdef NANO_STREAM
    const char *error = hb_ble_scan(found, &count, sender_beacon);
#else
    const char *error = hb_ble_scan(found, &count, NULL);
#endif

#ifdef NANO_STREAM
    unsigned mac_count = 0;
#endif
    for (unsigned i = 0; i < count; i++) {
        const hb_ble_device_t *device = &found[i];
        char label[96];
        uint8_t mac[6];
        if (mac_beacon_address(device->name, mac)) {
            lv_snprintf(label, sizeof(label), "Nano Bluetooth Link\n%02X:%02X:%02X:%02X:%02X:%02X",
                        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
#ifdef NANO_STREAM
            mac_count++;
#endif
        } else {
#ifdef NANO_STREAM
            continue;
#else
            const uint8_t *a = device->address;
            lv_snprintf(label, sizeof(label), "%s\n%s\n%02X:%02X:%02X:%02X:%02X:%02X %s",
                        device->name,
                        device->type ? device->type : "Unknown type",
                        (unsigned)a[1], (unsigned)a[2], (unsigned)a[3], (unsigned)a[4],
                        (unsigned)a[5], (unsigned)a[6], a[0] ? "Private" : "Public");
#endif
        }
        lv_obj_t *row = lv_list_add_button(device_list, NULL, label);
        lv_label_set_long_mode(lv_obj_get_child(row, 0), LV_LABEL_LONG_MODE_WRAP);
        lv_obj_add_event_cb(row, select_ble_device, LV_EVENT_CLICKED, (void *)device);
    }

    if (error)
        lv_label_set_text(status_label, error);
#ifdef NANO_STREAM
    else if (mac_count)
        lv_label_set_text(status_label, "Tap the Mac to start listening.");
    else
        lv_label_set_text(status_label, "No Mac found. Open the matching media tab in Nano Bluetooth Link.");
#else
    else if (count)
        lv_label_set_text_fmt(status_label, "%u named BLE devices.\nTap one to select it.", count);
    else
        lv_label_set_text(status_label, "No named BLE devices found.");
#endif
}

static void finish_ble_test(lv_timer_t *timer)
{
    (void)timer;
    scan_ble();
    stop_polling();
}

static void run_ble_test(lv_event_t *event)
{
    (void)event;
    if (scan_timer)
        return;

    /* Let the surface present the status before the bounded scan runs. */
    scan_timer = lv_timer_create(finish_ble_test, 100, NULL);
    if (!scan_timer) {
        lv_label_set_text(status_label, "Could not start BLE test.");
        return;
    }
#ifndef NANO_STREAM
    lv_obj_add_state(classic_button, LV_STATE_DISABLED);
#endif
    lv_obj_add_state(ble_button, LV_STATE_DISABLED);
    lv_obj_add_state(address_button, LV_STATE_DISABLED);
    clear_results();
#ifdef NANO_STREAM
    lv_label_set_text(status_label, "Looking for Nano Bluetooth Link…");
#else
    lv_label_set_text(status_label, "Scanning for BLE devices...\nWait 5 seconds.");
#endif
}

static int prepare_unload(void)
{
#ifndef NANO_STREAM
    disarm_classic_capture();
#else
    if (startup_timer) {
        lv_timer_delete(startup_timer);
        startup_timer = NULL;
    }
#ifdef NANO_VIDEO_STREAM
    if (!hb_h264_stop())
        return 0;
    video_stream_reset();
#endif
#endif
#ifndef NANO_STREAM
    close_classic_inbox(NULL);
#endif
    if (classic_entry.dialog)
        close_address_dialog(NULL);
    if (!hb_classic_prepare_unload()) {
        close_classic_pair_prompt();
        return 0;
    }
#ifdef NANO_STREAM
    return 1;
#else
    return hb_ble_prepare_unload();
#endif
}

#ifndef NANO_STREAM
static void end_notification_test(const char *status)
{
    hb_ble_disconnect();
    lv_label_set_text(connect_label, "Connect");
    lv_label_set_text(status_label, status);
    stop_polling();
}

static void poll_gatt(lv_timer_t *timer)
{
    hb_ble_update_t update;
    hb_ble_poll(&update);
    if (!hb_ble_connected()) {
        end_notification_test(update.status ? update.status : "BLE link closed.");
        return;
    }
    lv_timer_set_period(timer, update.poll_ms);
    if (update.status) {
        lv_label_set_text(status_label, update.status);
    } else if (update.length) {
        char message[HB_BLE_NOTIFICATION_LIMIT + 1];
        for (unsigned i = 0; i < update.length; i++) {
            uint8_t byte = update.data[i];
            message[i] = byte >= 32 && byte <= 126 ? (char)byte : ' ';
        }
        message[update.length] = 0;
        lv_label_set_text_fmt(status_label, "Received:\n%s", message);
    }
}

static void finish_connect_test(lv_timer_t *timer)
{
    const char *status = hb_ble_connect(selected_device->address);
    if (status) {
        lv_label_set_text(status_label, status);
        stop_polling();
        return;
    }
    hb_shared->prepare_unload = prepare_unload;
    lv_label_set_text(connect_label, "Disconnect");
    lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
    lv_label_set_text(status_label, "BLE link open. Discovering services...");
    lv_timer_set_cb(timer, poll_gatt);
}
#endif

static void close_classic_pair_prompt(void)
{
    if (classic_pair_prompt) {
        lv_msgbox_close(classic_pair_prompt);
        classic_pair_prompt = NULL;
    }
}

static void answer_classic_pair(lv_event_t *event)
{
    bool confirm = lv_event_get_user_data(event) != NULL;
    hb_classic_pair_reply(classic_pair_value, confirm);
    close_classic_pair_prompt();
}

#ifndef NANO_STREAM
static void close_classic_inbox(lv_event_t *event)
{
    (void)event;
    if (classic_inbox) {
        lv_msgbox_close(classic_inbox);
        classic_inbox = NULL;
    }
}

static void play_classic_file(lv_event_t *event)
{
    lv_obj_t *list = lv_event_get_user_data(event);
    lv_obj_t *row = lv_event_get_target(event);
    const char *name = lv_list_get_button_text(list, row);
    char path[sizeof(CLASSIC_INBOX) + CLASSIC_FILENAME_CAPACITY];
    lv_snprintf(path, sizeof(path), CLASSIC_INBOX "/%s", name);
    const char *error = hb_fs_size(path) > CLASSIC_BUFFERED_PLAY_LIMIT
        ? "Open this file in Audio Player."
        : hb_pcm_play_wav_file(path, CLASSIC_BUFFERED_PLAY_LIMIT);
    if (error) {
        lv_label_set_text(classic_inbox_status, error);
    } else {
        lv_label_set_text_fmt(classic_inbox_status, "Playing %s", name);
    }
}

static void open_classic_inbox(lv_event_t *event)
{
    (void)event;
    if (!hb_shared || !firmware_matches()) {
        lv_label_set_text(status_label, "Playback needs firmware 1.1.2 (39A10023).");
        return;
    }
    if (hb_ble_connected()) {
        lv_label_set_text(status_label, "Disconnect BLE to play saved files.");
        return;
    }
    hb_shared->prepare_unload = prepare_unload;
    classic_inbox = lv_msgbox_create(NULL);
    lv_obj_set_width(classic_inbox, 224);
    lv_msgbox_add_title(classic_inbox, "Received audio");
    classic_inbox_status = lv_msgbox_add_text(classic_inbox, "Tap a file to play it.");
    lv_obj_t *list = lv_list_create(lv_msgbox_get_content(classic_inbox));
    lv_obj_set_size(list, LV_PCT(100), 156);
    bool found = false;
    hb_dir_t dir;
    if (hb_fs_is_dir(CLASSIC_INBOX) && hb_fs_dir_open(&dir, CLASSIC_INBOX, false)) {
        char name[CLASSIC_FILENAME_CAPACITY];
        char pending[CLASSIC_FILENAME_CAPACITY];
        bool has_pending = classic_inbox_pending_name(pending);
        bool is_dir;
        while (hb_fs_dir_next(&dir, name, sizeof(name), &is_dir)) {
            if (is_dir || !classic_inbox_wav_name(name) || (has_pending && !strcmp(name, pending)))
                continue;
            lv_obj_t *row = lv_list_add_button(list, NULL, name);
            lv_obj_add_event_cb(row, play_classic_file, LV_EVENT_CLICKED, list);
            found = true;
        }
        hb_fs_dir_close(&dir);
    }
    if (!found) {
        lv_obj_add_flag(list, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(classic_inbox_status, "No WAV files in the Classic inbox.");
    }
    lv_obj_t *close = lv_msgbox_add_footer_button(classic_inbox, "Close");
    lv_obj_add_event_cb(close, close_classic_inbox, LV_EVENT_CLICKED, NULL);
}
#endif

#ifdef NANO_VIDEO_STREAM
static lv_obj_t *video_progress_bar, *video_progress_percent, *video_progress_spinner;

static void update_video_progress(void)
{
    int percent;
    bool active = hb_classic_video_progress(&percent);
    bool known = active && percent >= 0;
    lv_obj_set_flag(video_progress_bar, LV_OBJ_FLAG_HIDDEN, !known);
    lv_obj_set_flag(video_progress_percent, LV_OBJ_FLAG_HIDDEN, !known);
    lv_obj_set_flag(video_progress_spinner, LV_OBJ_FLAG_HIDDEN, !active || known);
    if (known && lv_bar_get_value(video_progress_bar) != percent) {
        lv_bar_set_value(video_progress_bar, percent, LV_ANIM_OFF);
        lv_label_set_text_fmt(video_progress_percent, "%d%%", percent);
    }
}
#endif

#ifdef NANO_YOUTUBE_SEARCH
static lv_obj_t *search_field, *search_keyboard, *search_button;
static lv_obj_t *result_card, *result_title, *result_artist;
static lv_obj_t *result_position, *previous_result, *next_result, *play_result_button;
static lv_obj_t *new_search_button, *cancel_search_button, *youtube_title;
#ifdef NANO_YOUTUBE_AUDIO
static lv_obj_t *result_art, *result_placeholder;
static uint16_t result_pixels[CLASSIC_THUMB_SIDE * CLASSIC_THUMB_SIDE] __attribute__((aligned(32)));
#endif
static unsigned result_index;
#ifdef NANO_YOUTUBE_VIDEO
#define SEARCH_PROMPT "Search YouTube."
#define CHOOSE_PROMPT "Choose a video."
#define SEARCH_PROGRESS "Searching YouTube..."
#define PLAY_PROGRESS "Waiting for the Mac to start the video..."
#else
#define SEARCH_PROMPT "Search YouTube Music."
#define CHOOSE_PROMPT "Choose a song."
#define SEARCH_PROGRESS "Searching YouTube Music..."
#define PLAY_PROGRESS "Downloading selected song..."
static lv_obj_t *search_cover;
static lv_obj_t *audio_controls, *audio_pause_label, *audio_clock;
static uint16_t cover_pixels[CLASSIC_COVER_SIDE * CLASSIC_COVER_SIDE] __attribute__((aligned(32)));

static void toggle_audio_pause(lv_event_t *event)
{
    (void)event;
    hb_classic_audio_toggle_pause();
}

static void stop_youtube_audio(lv_event_t *event)
{
    (void)event;
    hb_classic_audio_stop();
}

static void update_audio_controls(void)
{
    bool playing = hb_classic_audio_playing();
    lv_obj_set_flag(audio_controls, LV_OBJ_FLAG_HIDDEN, !playing);
    if (!playing)
        return;
    const char *action = hb_classic_audio_paused() ? "Play" : "Pause";
    if (strcmp(lv_label_get_text(audio_pause_label), action))
        lv_label_set_text(audio_pause_label, action);
    unsigned seconds = hb_classic_audio_elapsed_seconds();
    char elapsed[20];
    lv_snprintf(elapsed, sizeof(elapsed), "%u:%02u", seconds / 60u, seconds % 60u);
    if (strcmp(lv_label_get_text(audio_clock), elapsed))
        lv_label_set_text(audio_clock, elapsed);
}
#endif
static struct classic_song_list songs;
static bool editing_search, typing_landscape;
static uint32_t search_error_at;
static bool search_error_visible;

static void submit_search(lv_event_t *event)
{
    (void)event;
    const char *query = lv_textarea_get_text(search_field);
    while (*query == ' ')
        query++;
    if (!*query)
        return;
    unsigned error = hb_classic_search_submit(query);
    search_error_visible = error != 0;
    if (error) {
        search_error_at = lv_tick_get();
        if (error == 0x1d8u || error == 2u)
            lv_label_set_text_fmt(status_label, "Search send failed (0x%X). MTU %u.",
                                  error, hb_classic_search_mtu());
        else if (error == 0x19eu)
            lv_label_set_text(status_label, "Classic send blocked (0x19E). Try Search on the Mac.");
        else
            lv_label_set_text_fmt(status_label, "Could not send search (0x%X).", error);
    } else {
        editing_search = false;
        lv_label_set_text(status_label, SEARCH_PROGRESS);
    }
}

static void play_result(lv_event_t *event)
{
    (void)event;
    unsigned error = hb_classic_search_play(songs.revision, result_index);
    search_error_visible = error != 0;
    if (error) {
        search_error_at = lv_tick_get();
        lv_label_set_text_fmt(status_label, "Could not select result (0x%X).", error);
    } else {
        lv_label_set_text(status_label, PLAY_PROGRESS);
    }
}

static void show_result(void)
{
    if (result_index >= songs.count)
        return;
    lv_label_set_text(result_title, songs.titles[result_index]);
    const char *artist = songs.artists[result_index];
#ifdef NANO_YOUTUBE_VIDEO
    const char *unknown = "Unknown author";
#else
    const char *unknown = "Unknown artist";
#endif
    lv_label_set_text(result_artist, *artist ? artist : unknown);
    lv_label_set_text_fmt(result_position, "%u / %u", result_index + 1u, songs.count);
    lv_obj_set_state(previous_result, LV_STATE_DISABLED, !result_index);
    lv_obj_set_state(next_result, LV_STATE_DISABLED, result_index + 1u >= songs.count);
#ifdef NANO_YOUTUBE_AUDIO
    bool artwork = hb_classic_search_artwork(songs.revision, result_index, result_pixels);
    lv_obj_set_flag(result_art, LV_OBJ_FLAG_HIDDEN, !artwork);
    lv_obj_set_flag(result_placeholder, LV_OBJ_FLAG_HIDDEN, artwork);
    if (artwork)
        lv_obj_invalidate(result_art);
#endif
}

static void step_result(lv_event_t *event)
{
    int direction = 0;
    if (lv_event_get_code(event) == LV_EVENT_GESTURE) {
        lv_dir_t gesture = lv_indev_get_gesture_dir(lv_indev_active());
        if (gesture == LV_DIR_LEFT)
            direction = 1;
        else if (gesture == LV_DIR_RIGHT)
            direction = -1;
    } else {
        direction = lv_event_get_target(event) == previous_result ? -1 : 1;
    }
    int index = (int)result_index + direction;
    if (direction && index >= 0 && (unsigned)index < songs.count) {
        result_index = (unsigned)index;
        show_result();
    }
}

static void edit_youtube_search(lv_event_t *event)
{
    (void)event;
    editing_search = true;
    search_error_visible = false;
    lv_label_set_text(status_label, SEARCH_PROMPT);
}

static void cancel_youtube_search(lv_event_t *event)
{
    (void)event;
    editing_search = false;
    search_error_visible = false;
}

static void update_youtube_search(void)
{
    if (hb_classic_search_results(&songs)) {
        editing_search = false;
        result_index = 0;
        show_result();
    }
    bool ready = hb_classic_search_ready();
    if (!ready)
        editing_search = false;
    if (typing_landscape != editing_search) {
        typing_landscape = editing_search;
        hb_lv_set_landscape(typing_landscape);
        if (typing_landscape) {
            lv_obj_set_width(status_label, 300);
            lv_obj_align(status_label, LV_ALIGN_TOP_LEFT, 12, 18);
        } else {
            lv_obj_set_width(status_label, 208);
            lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 93);
        }
    }
    bool show_songs = ready && songs.count && !typing_landscape;
    lv_obj_set_flag(youtube_title, LV_OBJ_FLAG_HIDDEN, typing_landscape);
    lv_obj_set_flag(connect_button, LV_OBJ_FLAG_HIDDEN, typing_landscape);
    lv_obj_set_flag(result_card, LV_OBJ_FLAG_HIDDEN, !show_songs);
    lv_obj_set_flag(play_result_button, LV_OBJ_FLAG_HIDDEN, !show_songs);
    int search_width = show_songs ? 100 : 170;
    if (lv_obj_get_style_width(new_search_button, LV_PART_MAIN) != search_width) {
        lv_obj_set_width(new_search_button, search_width);
        lv_obj_align(new_search_button, LV_ALIGN_TOP_MID, show_songs ? 54 : 0, 319);
    }
    lv_obj_set_flag(new_search_button, LV_OBJ_FLAG_HIDDEN, !ready || typing_landscape);
    lv_obj_set_flag(cancel_search_button, LV_OBJ_FLAG_HIDDEN, !typing_landscape);
    lv_obj_set_flag(search_field, LV_OBJ_FLAG_HIDDEN, !typing_landscape);
    lv_obj_set_flag(search_keyboard, LV_OBJ_FLAG_HIDDEN, !typing_landscape);
    lv_obj_set_flag(search_button, LV_OBJ_FLAG_HIDDEN, !typing_landscape);
}
#endif

static void poll_classic_link(lv_timer_t *timer)
{
    (void)timer;
    uint8_t peer[6];
    if (!classic_peer_saved && hb_classic_confirmed_peer(peer)) {
        classic_peer_saved = true;
        if (valid_classic_address(peer) &&
            (!classic_remembered || memcmp(peer, remembered_classic, sizeof(peer)))) {
            memcpy(remembered_classic, peer, sizeof(peer));
            classic_remembered = hb_fs_write(CLASSIC_PEER_PATH, peer, sizeof(peer));
        }
    }
    unsigned code;
    if (hb_classic_pair_code(&code)) {
        if (!classic_pair_prompt || classic_pair_value != code) {
#ifndef NANO_STREAM
            close_classic_inbox(NULL);
#endif
            close_classic_pair_prompt();
            classic_pair_value = code;
            classic_pair_prompt = lv_msgbox_create(NULL);
            lv_obj_set_width(classic_pair_prompt, 220);
            lv_msgbox_add_title(classic_pair_prompt, "Pair with the Mac");
            char prompt[96];
            lv_snprintf(prompt, sizeof(prompt), "Code: %06u\nDo both devices show this code?", code);
            lv_msgbox_add_text(classic_pair_prompt, prompt);
            lv_obj_t *cancel = lv_msgbox_add_footer_button(classic_pair_prompt, "Cancel");
            lv_obj_t *match = lv_msgbox_add_footer_button(classic_pair_prompt, "Match");
            lv_obj_add_event_cb(cancel, answer_classic_pair, LV_EVENT_CLICKED, NULL);
            lv_obj_add_event_cb(match, answer_classic_pair, LV_EVENT_CLICKED, match);
        }
    } else {
        close_classic_pair_prompt();
    }
    const char *status = hb_classic_poll();
#ifdef NANO_YOUTUBE_AUDIO
    if (hb_classic_audio_playing() && hb_classic_audio_paused())
        status = "Paused";
#endif
#ifdef NANO_YOUTUBE_SEARCH
    if (search_error_visible && (!hb_classic_search_ready() || lv_tick_elaps(search_error_at) >= 8000))
        search_error_visible = false;
    if (editing_search && hb_classic_search_ready() && !strcmp(status, CHOOSE_PROMPT))
        status = SEARCH_PROMPT;
    if (!search_error_visible && strcmp(lv_label_get_text(status_label), status))
        lv_label_set_text(status_label, status);
#else
    if (strcmp(lv_label_get_text(status_label), status))
        lv_label_set_text(status_label, status);
#endif
#ifdef NANO_YOUTUBE_AUDIO
    update_audio_controls();
    bool cover_hidden = lv_obj_has_flag(search_cover, LV_OBJ_FLAG_HIDDEN);
    if (hb_classic_search_cover(cover_pixels, cover_hidden)) {
        if (cover_hidden) {
            lv_obj_remove_flag(search_cover, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(search_cover);
        }
    } else if (!cover_hidden) {
        lv_obj_add_flag(search_cover, LV_OBJ_FLAG_HIDDEN);
    }
#endif
#ifdef NANO_YOUTUBE_SEARCH
    update_youtube_search();
#endif
#ifdef NANO_VIDEO_STREAM
    update_video_progress();
#endif
    if (!hb_classic_busy()) {
#ifdef NANO_VIDEO_STREAM
        video_stream_reset();
#endif
        close_classic_pair_prompt();
        classic_closing = false;
        lv_label_set_text(connect_label, LISTEN_LABEL);
        stop_polling();
    }
}

static void begin_classic_link(void)
{
#ifdef NANO_YOUTUBE_AUDIO
    hb_trace_log("YTBTBEG ", 0, 0);
#endif
    unsigned poll_ms = 25;
#ifdef NANO_VIDEO_STREAM
    // Live video converts at most one frame per poll. Keep the shorter
    // interval so all frames in a group can be converted before playback.
    poll_ms = 8;
#endif
    scan_timer = lv_timer_create(poll_classic_link, poll_ms, NULL);
    if (!scan_timer || !hb_shared) {
        if (scan_timer)
            stop_polling();
        lv_label_set_text(status_label, "Could not start Classic connection.");
        return;
    }
    unsigned error = hb_classic_listen_media(selected_classic);
    if (error) {
        stop_polling();
        lv_label_set_text_fmt(status_label, "Classic listen error 0x%04X (%u)", error, error);
        return;
    }
    classic_peer_saved = false;
#ifdef NANO_VIDEO_STREAM
    video_stream_reset();
#endif
    hb_shared->prepare_unload = prepare_unload;
    hb_classic_enable_pairing();
#ifndef NANO_STREAM
    lv_obj_add_state(classic_button, LV_STATE_DISABLED);
#endif
    lv_obj_add_state(ble_button, LV_STATE_DISABLED);
    lv_obj_add_state(address_button, LV_STATE_DISABLED);
#ifdef NANO_STREAM
    lv_obj_add_flag(ble_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(address_button, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(device_list, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(connect_label, "Stop");
#else
    lv_label_set_text(connect_label, "Stop Classic");
#endif
    lv_label_set_text(status_label, hb_classic_poll());
#ifdef NANO_YOUTUBE_AUDIO
    hb_trace_log("YTBTEND ", 0, 0);
#endif
}

static void close_address_dialog(lv_event_t *event)
{
    (void)event;
    lv_msgbox_close(classic_entry.dialog);
    classic_entry.dialog = NULL;
}

static void listen_for_entered_address(lv_event_t *event)
{
    if (classic_entry.digits != sizeof(classic_entry.hex))
        return;
    for (unsigned i = 0; i < sizeof(classic_entry.address); i++) {
        unsigned high = hex_value(classic_entry.hex[i * 2]);
        unsigned low = hex_value(classic_entry.hex[i * 2 + 1]);
        classic_entry.address[i] = (uint8_t)(high << 4 | low);
    }
    close_address_dialog(event);
    select_classic_address(classic_entry.address);
    begin_classic_link();
}

static void enter_address_digit(lv_event_t *event)
{
    lv_obj_t *keypad = lv_event_get_target(event);
    const char *key = lv_buttonmatrix_get_button_text(
        keypad, lv_buttonmatrix_get_selected_button(keypad));
    if (!key)
        return;
    if (!strcmp(key, LV_SYMBOL_BACKSPACE)) {
        if (classic_entry.digits)
            classic_entry.hex[--classic_entry.digits] = '_';
    } else if (classic_entry.digits < sizeof(classic_entry.hex)) {
        classic_entry.hex[classic_entry.digits++] = key[0];
    }
    char display[18];
    for (unsigned i = 0; i < sizeof(classic_entry.address); i++) {
        display[i * 3] = classic_entry.hex[i * 2];
        display[i * 3 + 1] = classic_entry.hex[i * 2 + 1];
        display[i * 3 + 2] = i == 5 ? '\0' : ':';
    }
    lv_label_set_text(classic_entry.text, display);
    if (classic_entry.digits == sizeof(classic_entry.hex))
        lv_obj_remove_state(classic_entry.listen, LV_STATE_DISABLED);
    else
        lv_obj_add_state(classic_entry.listen, LV_STATE_DISABLED);
}

static void open_address_dialog(lv_event_t *event)
{
    (void)event;
    if (classic_entry.dialog || (scan_timer && !scan_client))
        return;
    static const char *const keys[] = {
        "0", "1", "2", "3", "\n", "4", "5", "6", "7", "\n",
        "8", "9", "A", "B", "\n", "C", "D", "E", "F", "\n",
        LV_SYMBOL_BACKSPACE, ""
    };
    classic_entry.dialog = lv_msgbox_create(NULL);
    if (!classic_entry.dialog)
        return;
    if (hb_shared)
        hb_shared->prepare_unload = prepare_unload;
    lv_obj_set_width(classic_entry.dialog, 224);
    lv_msgbox_add_title(classic_entry.dialog, "Mac Classic address");
    classic_entry.text = lv_msgbox_add_text(classic_entry.dialog, "__:__:__:__:__:__");
    classic_entry.digits = 0;
    memset(classic_entry.hex, '_', sizeof(classic_entry.hex));
    lv_obj_t *keypad = lv_buttonmatrix_create(lv_msgbox_get_content(classic_entry.dialog));
    lv_obj_set_size(keypad, LV_PCT(100), 190);
    lv_buttonmatrix_set_map(keypad, keys);
    lv_obj_add_event_cb(keypad, enter_address_digit, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *cancel = lv_msgbox_add_footer_button(classic_entry.dialog, "Cancel");
    classic_entry.listen = lv_msgbox_add_footer_button(classic_entry.dialog, "Listen");
    lv_obj_add_state(classic_entry.listen, LV_STATE_DISABLED);
    lv_obj_add_event_cb(cancel, close_address_dialog, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(classic_entry.listen, listen_for_entered_address, LV_EVENT_CLICKED, NULL);
}

static void begin_connect_test(lv_event_t *event)
{
    (void)event;
#ifndef NANO_STREAM
    if (hb_ble_connected()) {
        end_notification_test("BLE disconnected.");
        return;
    }
#endif
    if (selected_classic && hb_classic_busy() && !classic_closing) {
        classic_closing = true;
        hb_classic_disconnect();
        return;
    }
    if (scan_timer)
        return;
    if (selected_classic) {
        begin_classic_link();
        return;
    }
#ifndef NANO_STREAM
    if (!selected_device)
        return;

    scan_timer = lv_timer_create(finish_connect_test, 100, NULL);
    if (!scan_timer) {
        lv_label_set_text(status_label, "Could not start BLE connection test.");
        return;
    }
    lv_obj_add_state(classic_button, LV_STATE_DISABLED);
    lv_obj_add_state(ble_button, LV_STATE_DISABLED);
    lv_obj_add_state(address_button, LV_STATE_DISABLED);
    lv_obj_add_state(connect_button, LV_STATE_DISABLED);
    lv_label_set_text_fmt(status_label, "Connecting to %s...", selected_device->name);
#endif
}

#ifdef NANO_STREAM
static void load_saved_mac(lv_timer_t *timer)
#else
static void load_saved_mac(void)
#endif
{
#ifdef NANO_STREAM
    (void)timer;
    startup_timer = NULL;
    if (scan_timer || classic_entry.dialog || selected_classic)
        return;
#endif
    uint8_t saved[7];
    if (hb_fs_read(CLASSIC_PEER_PATH, saved, sizeof(saved)) != sizeof(remembered_classic) ||
        !valid_classic_address(saved))
        return;

    memcpy(remembered_classic, saved, sizeof(remembered_classic));
    classic_remembered = true;
    add_remembered_row();
    select_classic_address(remembered_classic);
#if defined(NANO_STREAM) && !defined(NANO_YOUTUBE_AUDIO) && !defined(NANO_YOUTUBE_VIDEO)
    begin_classic_link();
#endif
}

HB_APP_ENTRY(payload_entry)
{
#ifdef NANO_YOUTUBE_AUDIO
    hb_trace_log("YTUIBEG ", 0, 0);
#endif
    lv_obj_t *screen = lv_screen_active();

    lv_obj_t *title = lv_label_create(screen);
#ifdef NANO_STREAM
    if (hb_shared) {
        hb_classic_init_media();
        hb_shared->prepare_unload = prepare_unload;
    }
#if defined(NANO_YOUTUBE_VIDEO)
    lv_label_set_text(title, "YT Video");
#elif defined(NANO_VIDEO_STREAM)
    lv_label_set_text(title, "VideoStream");
#else
#ifdef NANO_YOUTUBE_AUDIO
    lv_label_set_text(title, "YT Music");
#else
    lv_label_set_text(title, "Stream");
#endif
#endif
#else
    lv_label_set_text(title, "Bluetooth Test");
#endif
#ifdef NANO_YOUTUBE_SEARCH
    youtube_title = title;
#endif
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 208);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
#ifdef NANO_VIDEO_STREAM
    video_progress_bar = lv_bar_create(screen);
    lv_obj_set_size(video_progress_bar, 192, 8);
    lv_obj_align(video_progress_bar, LV_ALIGN_TOP_MID, 0, 146);
    lv_obj_add_flag(video_progress_bar, LV_OBJ_FLAG_HIDDEN);
    video_progress_percent = lv_label_create(screen);
    lv_label_set_text(video_progress_percent, "0%");
    lv_obj_align(video_progress_percent, LV_ALIGN_TOP_MID, 0, 159);
    lv_obj_add_flag(video_progress_percent, LV_OBJ_FLAG_HIDDEN);
    video_progress_spinner = lv_spinner_create(screen);
    lv_obj_set_size(video_progress_spinner, 23, 23);
    lv_obj_align(video_progress_spinner, LV_ALIGN_TOP_MID, 0, 145);
    lv_obj_add_flag(video_progress_spinner, LV_OBJ_FLAG_HIDDEN);
#endif
#ifdef NANO_STREAM
    lv_label_set_text(status_label, "Open Nano Bluetooth Link on the Mac.");
#else
    lv_label_set_text(status_label, "Choose a Bluetooth test.");
#endif
#ifdef NANO_STREAM
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 93);

    ble_button = lv_button_create(screen);
    lv_obj_set_size(ble_button, 100, 40);
    lv_obj_align(ble_button, LV_ALIGN_TOP_MID, -54, 175);
    lv_obj_add_event_cb(ble_button, run_ble_test, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ble_label = lv_label_create(ble_button);
    lv_label_set_text(ble_label, "Find Mac");
    lv_obj_center(ble_label);

    address_button = lv_button_create(screen);
    lv_obj_set_size(address_button, 100, 40);
    lv_obj_align(address_button, LV_ALIGN_TOP_MID, 54, 175);
    lv_obj_add_event_cb(address_button, open_address_dialog, LV_EVENT_CLICKED, NULL);
    lv_obj_t *address_label = lv_label_create(address_button);
    lv_label_set_text(address_label, "Mac Address");
    lv_obj_center(address_label);

    device_list = lv_list_create(screen);
    lv_obj_set_size(device_list, 208, 130);
    lv_obj_align(device_list, LV_ALIGN_TOP_MID, 0, 229);
    lv_obj_add_flag(device_list, LV_OBJ_FLAG_HIDDEN);
#else
    lv_obj_align(status_label, LV_ALIGN_TOP_MID, 0, 133);

    classic_button = lv_button_create(screen);
    lv_obj_set_size(classic_button, 100, 40);
    lv_obj_align(classic_button, LV_ALIGN_TOP_MID, -54, 76);
    lv_obj_add_event_cb(classic_button, begin_scan, LV_EVENT_CLICKED, NULL);

    lv_obj_t *classic_label = lv_label_create(classic_button);
    lv_label_set_text(classic_label, "Scan Classic");
    lv_obj_center(classic_label);

    ble_button = lv_button_create(screen);
    lv_obj_set_size(ble_button, 100, 40);
    lv_obj_align(ble_button, LV_ALIGN_TOP_MID, 54, 76);
    lv_obj_add_event_cb(ble_button, run_ble_test, LV_EVENT_CLICKED, NULL);

    lv_obj_t *ble_label = lv_label_create(ble_button);
    lv_label_set_text(ble_label, "Scan BLE");
    lv_obj_center(ble_label);

    address_button = lv_button_create(screen);
    lv_obj_set_size(address_button, 108, 32);
    lv_obj_align(address_button, LV_ALIGN_TOP_MID, -50, 189);
    lv_obj_add_event_cb(address_button, open_address_dialog, LV_EVENT_CLICKED, NULL);
    lv_obj_t *address_label = lv_label_create(address_button);
    lv_label_set_text(address_label, "Mac Address");
    lv_obj_center(address_label);

    lv_obj_t *play_button = lv_button_create(screen);
    lv_obj_set_size(play_button, 92, 32);
    lv_obj_align(play_button, LV_ALIGN_TOP_MID, 58, 189);
    lv_obj_add_event_cb(play_button, open_classic_inbox, LV_EVENT_CLICKED, NULL);
    lv_obj_t *play_label = lv_label_create(play_button);
    lv_label_set_text(play_label, "Play Files");
    lv_obj_center(play_label);

    device_list = lv_list_create(screen);
    lv_obj_set_size(device_list, 208, 122);
    lv_obj_align(device_list, LV_ALIGN_TOP_MID, 0, 229);

#endif

    connect_button = lv_button_create(screen);
    lv_obj_set_size(connect_button, 208, 40);
#ifdef NANO_STREAM
    lv_obj_align(connect_button, LV_ALIGN_TOP_MID, 0, 372);
#else
    lv_obj_align(connect_button, LV_ALIGN_TOP_MID, 0, 360);
#endif
    lv_obj_add_state(connect_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(connect_button, begin_connect_test, LV_EVENT_CLICKED, NULL);

    connect_label = lv_label_create(connect_button);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
#ifdef NANO_YOUTUBE_AUDIO
    search_cover = lv_canvas_create(screen);
    lv_canvas_set_buffer(search_cover, cover_pixels, CLASSIC_COVER_SIDE, CLASSIC_COVER_SIDE,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_align(search_cover, LV_ALIGN_TOP_MID, 0, 156);
    lv_obj_add_flag(search_cover, LV_OBJ_FLAG_HIDDEN);

    audio_controls = lv_obj_create(screen);
    lv_obj_remove_style_all(audio_controls);
    lv_obj_remove_flag(audio_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(audio_controls, 208, 46);
    lv_obj_align(audio_controls, LV_ALIGN_TOP_MID, 0, 318);

    lv_obj_t *pause_audio = lv_button_create(audio_controls);
    lv_obj_set_size(pause_audio, 74, 38);
    lv_obj_align(pause_audio, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(pause_audio, toggle_audio_pause, LV_EVENT_CLICKED, NULL);
    audio_pause_label = lv_label_create(pause_audio);
    lv_label_set_text(audio_pause_label, "Pause");
    lv_obj_center(audio_pause_label);

    audio_clock = lv_label_create(audio_controls);
    lv_label_set_text(audio_clock, "0:00");
    lv_obj_center(audio_clock);

    lv_obj_t *stop_audio = lv_button_create(audio_controls);
    lv_obj_set_size(stop_audio, 74, 38);
    lv_obj_align(stop_audio, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(stop_audio, stop_youtube_audio, LV_EVENT_CLICKED, NULL);
    lv_obj_t *stop_label = lv_label_create(stop_audio);
    lv_label_set_text(stop_label, "Stop");
    lv_obj_center(stop_label);
    lv_obj_add_flag(audio_controls, LV_OBJ_FLAG_HIDDEN);
#endif
#ifdef NANO_YOUTUBE_SEARCH
    search_field = lv_textarea_create(screen);
    lv_obj_set_size(search_field, 306, 40);
    lv_obj_align(search_field, LV_ALIGN_TOP_LEFT, 12, 77);
    lv_textarea_set_one_line(search_field, true);
    lv_textarea_set_max_length(search_field, CLASSIC_SEARCH_LIMIT);
#ifdef NANO_YOUTUBE_VIDEO
    lv_textarea_set_placeholder_text(search_field, "Video or author");
#else
    lv_textarea_set_placeholder_text(search_field, "Song or artist");
#endif
    lv_obj_add_flag(search_field, LV_OBJ_FLAG_HIDDEN);

    search_button = lv_button_create(screen);
    lv_obj_set_size(search_button, 94, 40);
    lv_obj_align(search_button, LV_ALIGN_TOP_RIGHT, -12, 77);
    lv_obj_add_event_cb(search_button, submit_search, LV_EVENT_CLICKED, NULL);
    lv_obj_t *search_label = lv_label_create(search_button);
    lv_label_set_text(search_label, "Search");
    lv_obj_center(search_label);
    lv_obj_add_flag(search_button, LV_OBJ_FLAG_HIDDEN);

    search_keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(search_keyboard, 424, 112);
    lv_obj_align(search_keyboard, LV_ALIGN_BOTTOM_MID, 0, -4);
    lv_keyboard_set_textarea(search_keyboard, search_field);
    lv_obj_add_event_cb(search_keyboard, submit_search, LV_EVENT_READY, NULL);
    lv_obj_add_event_cb(search_keyboard, cancel_youtube_search, LV_EVENT_CANCEL, NULL);
    lv_obj_add_flag(search_keyboard, LV_OBJ_FLAG_HIDDEN);

    result_card = lv_obj_create(screen);
#ifdef NANO_YOUTUBE_VIDEO
    lv_obj_set_size(result_card, 208, 134);
    lv_obj_align(result_card, LV_ALIGN_TOP_MID, 0, 150);
#else
    lv_obj_set_size(result_card, 208, 190);
    lv_obj_align(result_card, LV_ALIGN_TOP_MID, 0, 122);
#endif
    lv_obj_set_style_pad_all(result_card, 0, 0);
    lv_obj_remove_flag(result_card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(result_card, LV_OBJ_FLAG_HIDDEN | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(result_card, step_result, LV_EVENT_GESTURE, NULL);

#ifdef NANO_YOUTUBE_AUDIO
    result_art = lv_canvas_create(result_card);
    lv_canvas_set_buffer(result_art, result_pixels, CLASSIC_THUMB_SIDE, CLASSIC_THUMB_SIDE,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_align(result_art, LV_ALIGN_TOP_MID, 0, 7);

    result_placeholder = lv_label_create(result_card);
    lv_label_set_text(result_placeholder, LV_SYMBOL_IMAGE);
    lv_obj_align(result_placeholder, LV_ALIGN_TOP_MID, 0, 46);
    lv_obj_set_style_text_opa(result_placeholder, LV_OPA_50, 0);
#endif

    previous_result = lv_button_create(result_card);
    next_result = lv_button_create(result_card);
    lv_obj_set_size(previous_result, 42, 44);
    lv_obj_set_size(next_result, 42, 44);
#ifdef NANO_YOUTUBE_VIDEO
    lv_obj_align(previous_result, LV_ALIGN_BOTTOM_LEFT, 5, -6);
    lv_obj_align(next_result, LV_ALIGN_BOTTOM_RIGHT, -5, -6);
#else
    lv_obj_align(previous_result, LV_ALIGN_TOP_LEFT, 5, 33);
    lv_obj_align(next_result, LV_ALIGN_TOP_RIGHT, -5, 33);
#endif
    lv_obj_add_event_cb(previous_result, step_result, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(next_result, step_result, LV_EVENT_CLICKED, NULL);
    lv_obj_t *left = lv_label_create(previous_result);
    lv_obj_t *right = lv_label_create(next_result);
    lv_label_set_text(left, LV_SYMBOL_LEFT);
    lv_label_set_text(right, LV_SYMBOL_RIGHT);
    lv_obj_center(left);
    lv_obj_center(right);

    result_title = lv_label_create(result_card);
    lv_obj_set_size(result_title, 192, 39);
    lv_label_set_long_mode(result_title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(result_title, LV_TEXT_ALIGN_CENTER, 0);
#ifdef NANO_YOUTUBE_VIDEO
    lv_obj_align(result_title, LV_ALIGN_TOP_MID, 0, 12);
#else
    lv_obj_align(result_title, LV_ALIGN_TOP_MID, 0, 108);
#endif

    result_artist = lv_label_create(result_card);
    lv_obj_set_size(result_artist, 192, 20);
    lv_label_set_long_mode(result_artist, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(result_artist, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(result_artist, LV_OPA_70, 0);
#ifdef NANO_YOUTUBE_VIDEO
    lv_obj_align(result_artist, LV_ALIGN_TOP_MID, 0, 55);
#else
    lv_obj_align(result_artist, LV_ALIGN_TOP_MID, 0, 149);
#endif

    result_position = lv_label_create(result_card);
    lv_obj_set_style_text_opa(result_position, LV_OPA_70, 0);
#ifdef NANO_YOUTUBE_VIDEO
    lv_obj_align(result_position, LV_ALIGN_BOTTOM_MID, 0, -19);
#else
    lv_obj_align(result_position, LV_ALIGN_BOTTOM_MID, 0, -3);
#endif

    play_result_button = lv_button_create(screen);
    lv_obj_set_size(play_result_button, 100, 36);
    lv_obj_align(play_result_button, LV_ALIGN_TOP_MID, -54, 319);
    lv_obj_add_event_cb(play_result_button, play_result, LV_EVENT_CLICKED, NULL);
    lv_obj_t *play_result_label = lv_label_create(play_result_button);
    lv_label_set_text(play_result_label, LV_SYMBOL_PLAY " Play");
    lv_obj_center(play_result_label);
    lv_obj_add_flag(play_result_button, LV_OBJ_FLAG_HIDDEN);

    new_search_button = lv_button_create(screen);
    lv_obj_set_size(new_search_button, 170, 36);
    lv_obj_align(new_search_button, LV_ALIGN_TOP_MID, 0, 319);
    lv_obj_add_event_cb(new_search_button, edit_youtube_search, LV_EVENT_CLICKED, NULL);
    lv_obj_t *new_search_label = lv_label_create(new_search_button);
    lv_label_set_text(new_search_label, "Search");
    lv_obj_center(new_search_label);
    lv_obj_add_flag(new_search_button, LV_OBJ_FLAG_HIDDEN);

    cancel_search_button = lv_button_create(screen);
    lv_obj_set_size(cancel_search_button, 94, 36);
    lv_obj_align(cancel_search_button, LV_ALIGN_TOP_RIGHT, -12, 8);
    lv_obj_add_event_cb(cancel_search_button, cancel_youtube_search, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_search_label = lv_label_create(cancel_search_button);
    lv_label_set_text(cancel_search_label, "Cancel");
    lv_obj_center(cancel_search_label);
    lv_obj_add_flag(cancel_search_button, LV_OBJ_FLAG_HIDDEN);
#endif
#ifdef NANO_YOUTUBE_AUDIO
    hb_trace_log("YTUIEND ", 0, 0);
    startup_timer = lv_timer_create(load_saved_mac, 250, NULL);
    if (startup_timer)
        lv_timer_set_repeat_count(startup_timer, 1);
#elif defined(NANO_STREAM)
    startup_timer = lv_timer_create(load_saved_mac, 120, NULL);
    if (startup_timer)
        lv_timer_set_repeat_count(startup_timer, 1);
    else
        load_saved_mac(NULL);
#else
    load_saved_mac();
#endif
}
