/* Classic inquiry and BLE diagnostics on firmware 39A10023. */
#include "hb_sdk.h"
#include "hb_shared.h"
#include "ble_audio.h"
#include "lvgl/lvgl.h"
#include <string.h>

/* The nano 7 has 64 MiB of SDRAM starting at 0x08000000. */
#define SDRAM_START 0x08000000u
#define SDRAM_END   0x0c000000u
#define A2DP_TIMESTAMP_CODE 0x082b2bd6u
#define BT_CLIENT_VTABLE 0x087ed34cu
#define BT_CLIENT_OFFSET 0x28u
#define BT_CLIENT_MUTEX 0x44u
#define BT_CLIENT_TYPE 0x7cu
#define BT_CLIENT_SESSION 0x80u
#define BT_CLIENT_LOCAL_DEVICE 0x84u
#define BT_CLIENT_AGENT 0x88u
#define BT_CLIENT_START_SLOT 0x44u
#define BT_CLIENT_START 0x08455c2du
#define BT_CLIENT_LOCK 0x08424a8bu
#define BT_CLIENT_UNLOCK 0x08424a7du
#define BT_AGENT_GET_DEVICES 0x080cd1e5u
#define BT_DEVICE_GET_NAME 0x080ccf5du
#define BT_GET_MODULE_POWER 0x08417069u
#define BT_GET_TEST_MODE 0x080cd649u
#define BT_CLASSIC_MODULE 1u
/* The discovery agent accepts every Classic inquiry result for this mask. */
#define BT_ANY_SERVICE UINT32_MAX
#define SCAN_POLL_MS 250u
#define SCAN_WAIT_MS 30000u

#define BT_MODEL_SLOT 0x08812d7cu
#define BT_MODEL_VTABLE 0x087b57e0u
#define DEVICE_LIMIT 24u

#define LE_CENTRAL_SLOT 0x089702fcu
#define LE_CENTRAL_VTABLE 0x087ec870u
#define LE_REPORT_HANDLER 0x08438f18u
#define LE_START_SCAN 0x0843aa01u
#define LE_STOP_SCAN 0x0843aef9u
#define LE_GET_SESSION 0x083d57c1u
#define LE_DEVICE_MANAGER_SLOT 0x089702a4u
#define LE_DEVICE_MANAGER_VTABLE 0x087eb5f4u
#define LE_FIND_DEVICE 0x083d5c19u
#define LE_DEVICE_VTABLE 0x087ec824u
#define LE_RETAIN_DEVICE 0x083ec9ddu
#define LE_RELEASE_DEVICE 0x08404589u
#define LE_CONNECT 0x084595e7u
#define LE_SET_MODE_SLOT 0x10u
#define LE_GET_MODE_SLOT 0x14u
#define LE_READY_SLOT 0x38u
#define LE_SET_MODE 0x0845944du
#define LE_GET_MODE 0x08459431u
#define LE_MODE_IDLE 2u
#define LE_MODE_ACTIVE 3u
#define LE_READY_POLLS 8u
#define LE_LINK_CONNECTED 0x084152d9u
#define LE_LINK_PENDING 0x083d1d4du
#define LE_GET_BROKER 0x083da7a1u
#define LE_DISCONNECT 0x082b3d41u
#define LE_CONNECT_POLLS 24u
#define LE_CACHE_READ_LIMIT 256u
#define LE_SESSION_SLOT 0x08909180u
#define LE_SESSION_VTABLE 0x087ed81cu
#define LE_SESSION_NATIVE 0x9cu
#define LE_SESSION_MUTEX 0xa0u
#define LE_SESSION_HANDLE 0xc0u
#define LE_SESSION_LOCAL_DEVICE 0xc4u
#define LE_DELAY_US 0x084063afu
#define LE_SCAN_US 5000000u
#define LE_POLL_US 250000u
#define LE_CENTRAL_LOCK 0x0842c0a1u
#define LE_CENTRAL_UNLOCK 0x0842c0b5u
#define LE_MUTEX 0x18u
#define LE_ENABLED 0x10u
#define LE_SCAN_PENDING 0x3cu
#define LE_SCANNING 0x3eu
#define LE_SCAN_REQUEST_COUNT 0x68u
#define LE_CACHE_SENTINEL 0x80u
#define LE_CACHE_COUNT 0x84u
#define LE_TREE_ROOT 0x04u
#define LE_NODE_LEFT 0x08u
#define LE_NODE_RIGHT 0x0cu
#define LE_NODE_DEVICE 0x10u
#define LE_NODE_REPORTS 0x14u
#define LE_DEVICE_ADDRESS 0x1au
#define LE_ADDRESS_SIZE 7u
#define LE_REPORT_VTABLE 0x087ebc98u
#define LE_REPORT_NAME 0x0cu
#define LE_REPORT_SERVICES 0x38u
#define LE_REPORT_SLOTS 3u
#define LE_UUID_SIZE 18u
#define LE_SERVICE_LIMIT 16u
#define LE_NAME_SIZE 32u

typedef void (*firmware_lock_fn)(uintptr_t *guard, void *mutex);
typedef void (*firmware_unlock_fn)(uintptr_t *guard);
typedef int (*get_module_power_fn)(uintptr_t device, uint32_t module, uint32_t *on);
typedef int (*get_test_mode_fn)(uintptr_t device, uint32_t *enabled);
typedef int (*start_scan_fn)(uintptr_t client, uint32_t services);
typedef int (*get_devices_fn)(uintptr_t agent, uintptr_t *out,
                              uint32_t *count, uint32_t capacity);
typedef int (*get_device_text_fn)(uintptr_t device, char *out, uint32_t capacity);
typedef uintptr_t (*get_ble_session_fn)(void);
typedef int (*start_ble_scan_fn)(uintptr_t central, uintptr_t session,
                                 const uintptr_t *uuids, unsigned duplicates,
                                 unsigned foreground);
typedef int (*stop_ble_scan_fn)(uintptr_t central, uintptr_t session);
typedef void (*delay_us_fn)(uint32_t us);
typedef uintptr_t (*find_ble_device_fn)(uintptr_t manager, const uint8_t *address,
                                        unsigned create);
typedef uintptr_t (*retain_ble_device_fn)(uintptr_t *ref, uintptr_t device);
typedef uintptr_t (*release_ble_device_fn)(uintptr_t *ref);
typedef int (*connect_ble_fn)(uintptr_t owner, const uintptr_t *ref);
typedef void (*set_ble_mode_fn)(uintptr_t owner, unsigned mode);
typedef unsigned (*get_ble_mode_fn)(uintptr_t owner);
typedef int (*ble_link_state_fn)(uintptr_t central, uintptr_t device);
typedef uintptr_t (*get_ble_broker_fn)(void);
typedef void (*disconnect_ble_fn)(uintptr_t broker, uintptr_t session, uintptr_t device);

typedef struct {
    uint8_t address[LE_ADDRESS_SIZE];
    char name[LE_NAME_SIZE];
    const char *type;
} ble_device_t;

static lv_obj_t *status_label;
static lv_obj_t *device_list;
static lv_obj_t *classic_button;
static lv_obj_t *ble_button;
static lv_obj_t *connect_button;
static lv_obj_t *connect_label;
static lv_obj_t *selected_row;
static const ble_device_t *selected_device;
static lv_timer_t *scan_timer;
static uintptr_t scan_client;
static uint32_t scan_started_ms;
static bool nearby_query_succeeded;
static uintptr_t nearby_devices[DEVICE_LIMIT];
static unsigned nearby_count;

static bool firmware_matches(void)
{
    /* Accept the stock instructions or NanoApps' existing timestamp fix. */
    const volatile uint16_t *code = (const volatile uint16_t *)A2DP_TIMESTAMP_CODE;
    return (code[0] == 0xee01 && code[1] == 0xca10 &&
            code[2] == 0xed91 && code[3] == 0x0a01) ||
           (code[0] == 0x6848 && code[1] == 0xeb00 &&
            code[2] == 0x10cc && code[3] == 0x6048);
}

static bool in_sdram(uintptr_t address, uint32_t size)
{
    return address >= SDRAM_START && address < SDRAM_END &&
           size <= SDRAM_END - address;
}

static uintptr_t read_pointer(uintptr_t address)
{
    if (address % sizeof(uintptr_t) || !in_sdram(address, sizeof(uintptr_t)))
        return 0;
    return *(const volatile uintptr_t *)address;
}

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
    if (!in_sdram(client, BT_CLIENT_AGENT + sizeof(uintptr_t)))
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
    lv_timer_delete(scan_timer);
    scan_timer = NULL;
    scan_client = 0;
    lv_obj_remove_state(classic_button, LV_STATE_DISABLED);
    lv_obj_remove_state(ble_button, LV_STATE_DISABLED);
    if (selected_device)
        lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
}

static void clear_results(void)
{
    selected_row = NULL;
    selected_device = NULL;
    lv_obj_add_state(connect_button, LV_STATE_DISABLED);
    lv_obj_clean(device_list);
}

static void add_nearby_device(uintptr_t device)
{
    if (!device || nearby_count == DEVICE_LIMIT)
        return;
    for (unsigned i = 0; i < nearby_count; i++)
        if (nearby_devices[i] == device)
            return;

    char name[256] = { 0 };
    get_device_text_fn get_name = (get_device_text_fn)BT_DEVICE_GET_NAME;
    if (get_name(device, name, sizeof(name)) || !name[0])
        return;
    name[sizeof(name) - 1] = 0;
    bool has_name = false;
    for (char *p = name; *p; p++) {
        if ((unsigned char)*p < 32 || *p == 127)
            *p = ' ';
        has_name |= *p != ' ';
    }
    if (has_name) {
        nearby_devices[nearby_count++] = device;
        lv_list_add_text(device_list, name);
    }
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

    if (!poll_nearby_devices(scan_client)) {
        stop_polling();
        if (nearby_count) {
            lv_label_set_text_fmt(status_label, "Found %u Classic devices", nearby_count);
        } else {
            lv_label_set_text(status_label, nearby_query_succeeded
                                             ? "No named Classic devices found."
                                             : "Could not read nearby scan results.");
        }
    } else if (hb_time_uptime_ms() - scan_started_ms >= SCAN_WAIT_MS) {
        stop_polling();
        lv_label_set_text(status_label, "Scan still running. Try again shortly.");
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

    /* An active agent keeps the service mask from the scan that started it. */
    if (read_pointer(client + BT_CLIENT_AGENT)) {
        lv_label_set_text(status_label, "A scan is already running. Try again shortly.");
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
    lv_label_set_text(status_label, "Scanning for nearby Classic devices...");

    /* The stock client owns the agent and its callbacks through completion.
     * LVGL only polls it, so leaving this screen requires no app callback
     * to remain registered with the Bluetooth stack. */
    start_scan_fn start_scan = (start_scan_fn)BT_CLIENT_START;
    if (start_scan(client, BT_ANY_SERVICE))
        return;

    bool scan_started_elsewhere = read_pointer(client + BT_CLIENT_AGENT) != 0;
    stop_polling();
    lv_label_set_text(status_label, scan_started_elsewhere
                                   ? "A scan is already running. Try again shortly."
                                   : "Could not start scan.");
}

/* Service hints describe a capability the device advertises. */
static const char *ble_service_type(uint16_t uuid)
{
    switch (uuid) {
    case 0x1808: return "Glucose sensor";
    case 0x1809: return "Health thermometer";
    case 0x180d: return "Heart rate";
    case 0x1810: return "Blood pressure";
    case 0x1812: return "HID input";
    case 0x1814: return "Running sensor";
    case 0x1816:
    case 0x1818: return "Cycling sensor";
    case 0x181a: return "Environmental sensor";
    case 0x181d: return "Weight scale";
    case 0x1826: return "Fitness machine";
    case 0x184e: return "LE audio";
    default: return NULL;
    }
}

static void read_ble_report(uintptr_t report, ble_device_t *result)
{
    if (!in_sdram(report, LE_REPORT_SERVICES + 2 * sizeof(uintptr_t)) ||
        read_pointer(report) != LE_REPORT_VTABLE)
        return;

    uintptr_t name = read_pointer(report + LE_REPORT_NAME);
    if (name >= SDRAM_START + 4) {
        unsigned length = read_pointer(name - 4);
        if (length && length < LE_NAME_SIZE && in_sdram(name, length)) {
            char text[LE_NAME_SIZE];
            const volatile uint8_t *bytes = (const volatile uint8_t *)name;
            unsigned i = 0;
            while (i < length && bytes[i]) {
                uint8_t byte = bytes[i];
                text[i++] = byte < 32 || byte == 127 ? ' ' : (char)byte;
            }
            while (i && text[i - 1] == ' ')
                i--;
            if (i > strlen(result->name)) {
                memcpy(result->name, text, i);
                result->name[i] = 0;
            }
        }
    }

    if (result->type)
        return;

    uintptr_t first = read_pointer(report + LE_REPORT_SERVICES);
    uintptr_t end = read_pointer(report + LE_REPORT_SERVICES + sizeof(uintptr_t));
    if (end <= first || (end - first) % LE_UUID_SIZE)
        return;
    uintptr_t size = end - first;
    if (size > LE_SERVICE_LIMIT * LE_UUID_SIZE)
        size = LE_SERVICE_LIMIT * LE_UUID_SIZE;
    if (!in_sdram(first, size))
        return;

    for (uintptr_t offset = 0; offset < size; offset += LE_UUID_SIZE) {
        const volatile uint8_t *uuid = (const volatile uint8_t *)(first + offset);
        if (uuid[0] == 2 && uuid[1] == 0) {
            result->type = ble_service_type((uint16_t)(uuid[2] | (uuid[3] << 8)));
            if (result->type)
                break;
        }
    }
}

/* The stock report handler at 0x08438f18 inserts device-keyed entries
 * under the LE Central mutex. Copy the metadata while holding it. */
static bool read_ble_devices(uintptr_t central, ble_device_t devices[DEVICE_LIMIT],
                             unsigned limit, unsigned *count)
{
    uintptr_t sentinel = read_pointer(central + LE_CACHE_SENTINEL);
    if (sentinel % sizeof(uintptr_t) ||
        !in_sdram(sentinel, LE_TREE_ROOT + sizeof(uintptr_t)))
        return false;

    uintptr_t stack[32];
    unsigned depth = 0;
    uintptr_t node = read_pointer(sentinel + LE_TREE_ROOT);
    for (unsigned visited = 0; visited < limit && *count < DEVICE_LIMIT; visited++) {
        while (node) {
            if (depth == sizeof(stack) / sizeof(stack[0]) || node == sentinel ||
                node % sizeof(uintptr_t) ||
                !in_sdram(node, LE_NODE_REPORTS + sizeof(uintptr_t)))
                return false;
            stack[depth++] = node;
            node = read_pointer(node + LE_NODE_LEFT);
        }
        if (!depth)
            return false;

        node = stack[--depth];
        uintptr_t device = read_pointer(node + LE_NODE_DEVICE);
        if (!in_sdram(device, LE_DEVICE_ADDRESS + LE_ADDRESS_SIZE))
            return false;

        ble_device_t *result = &devices[*count];
        *result = (ble_device_t){ 0 };
        const volatile uint8_t *address = (const volatile uint8_t *)(device + LE_DEVICE_ADDRESS);
        for (unsigned i = 0; i < LE_ADDRESS_SIZE; i++)
            result->address[i] = address[i];

        uintptr_t reports = read_pointer(node + LE_NODE_REPORTS);
        if (in_sdram(reports, LE_REPORT_SLOTS * sizeof(uintptr_t))) {
            for (unsigned i = 0; i < LE_REPORT_SLOTS; i++)
                read_ble_report(read_pointer(reports + i * sizeof(uintptr_t)), result);
        }
        if (result->name[0])
            (*count)++;
        node = read_pointer(node + LE_NODE_RIGHT);
    }
    return true;
}

static unsigned poll_ble_devices(uintptr_t central, ble_device_t found[DEVICE_LIMIT],
                                 unsigned count, bool *saw_scan, bool *read_failed)
{
    firmware_lock_fn lock = (firmware_lock_fn)LE_CENTRAL_LOCK;
    firmware_unlock_fn unlock = (firmware_unlock_fn)LE_CENTRAL_UNLOCK;
    uintptr_t guard[2] = { 0 };
    static ble_device_t snapshot[DEVICE_LIMIT];
    unsigned copied = 0;

    lock(guard, (void *)(central + LE_MUTEX));
    const volatile uint8_t *state = (const volatile uint8_t *)central;
    if (state[LE_SCANNING]) {
        *saw_scan = true;
        unsigned total = read_pointer(central + LE_CACHE_COUNT);
        unsigned limit = total < LE_CACHE_READ_LIMIT ? total : LE_CACHE_READ_LIMIT;
        if (limit && !read_ble_devices(central, snapshot, limit, &copied)) {
            *read_failed = true;
            copied = 0;
        }
    }
    unlock(guard);

    for (unsigned i = 0; i < copied; i++) {
        const ble_device_t *device = &snapshot[i];
        unsigned j = 0;
        while (j < count && memcmp(device->address, found[j].address, LE_ADDRESS_SIZE))
            j++;
        if (j == count) {
            if (count < DEVICE_LIMIT)
                found[count++] = *device;
            continue;
        }

        if (strlen(device->name) > strlen(found[j].name))
            memcpy(found[j].name, device->name, sizeof(found[j].name));
        if (!found[j].type)
            found[j].type = device->type;
    }
    return count;
}

static void select_ble_device(lv_event_t *event)
{
    if (scan_timer)
        return;

    if (selected_row)
        lv_obj_remove_state(selected_row, LV_STATE_CHECKED);
    selected_row = lv_event_get_target_obj(event);
    selected_device = lv_event_get_user_data(event);
    lv_obj_add_state(selected_row, LV_STATE_CHECKED);
    lv_obj_remove_state(connect_button, LV_STATE_DISABLED);
    lv_label_set_text_fmt(status_label, "Selected %s", selected_device->name);
}

static void scan_ble(void)
{
    const volatile uint16_t *handler = (const volatile uint16_t *)LE_REPORT_HANDLER;
    const volatile uint16_t *start_code = (const volatile uint16_t *)(LE_START_SCAN & ~1u);
    const volatile uint16_t *stop_code = (const volatile uint16_t *)(LE_STOP_SCAN & ~1u);
    if (!firmware_matches() || handler[0] != 0xe92d || handler[1] != 0x4fff ||
        handler[2] != 0x4606 || handler[3] != 0xb095 ||
        start_code[0] != 0xe92d || start_code[1] != 0x43ff ||
        stop_code[0] != 0xe92d || stop_code[1] != 0x41f3) {
        lv_label_set_text(status_label, "BLE firmware signature mismatch.");
        return;
    }

    uintptr_t central = read_pointer(LE_CENTRAL_SLOT);
    if (!central) {
        lv_label_set_text(status_label, "Turn Bluetooth on in Settings first.");
        return;
    }
    if (!in_sdram(central, LE_CACHE_COUNT + sizeof(uintptr_t)) ||
        read_pointer(central) != LE_CENTRAL_VTABLE) {
        lv_label_set_text(status_label, "BLE controller table mismatch.");
        return;
    }

    const volatile uint8_t *state = (const volatile uint8_t *)central;
    if (!state[LE_ENABLED]) {
        lv_label_set_text(status_label, "Turn Bluetooth on in Settings.");
        return;
    }

    uintptr_t owner = read_pointer(LE_SESSION_SLOT);
    if (!owner) {
        get_ble_session_fn get_session = (get_ble_session_fn)LE_GET_SESSION;
        owner = get_session();
    }
    if (!in_sdram(owner, 0xccu) || read_pointer(owner) != LE_SESSION_VTABLE) {
        lv_label_set_text(status_label, "BLE session is unavailable.");
        return;
    }

    firmware_lock_fn lock_central = (firmware_lock_fn)LE_CENTRAL_LOCK;
    firmware_unlock_fn unlock_central = (firmware_unlock_fn)LE_CENTRAL_UNLOCK;
    firmware_lock_fn lock_session = (firmware_lock_fn)BT_CLIENT_LOCK;
    firmware_unlock_fn unlock_session = (firmware_unlock_fn)BT_CLIENT_UNLOCK;
    start_ble_scan_fn start = (start_ble_scan_fn)LE_START_SCAN;
    stop_ble_scan_fn stop = (stop_ble_scan_fn)LE_STOP_SCAN;
    delay_us_fn delay = (delay_us_fn)LE_DELAY_US;
    uintptr_t central_guard[2] = { 0 };
    uintptr_t session_guard = 0;

    /* Follow the stock lock order. The native start and stop routines take
     * the central lock themselves, so release it before calling either. */
    lock_session(&session_guard, (void *)(owner + LE_SESSION_MUTEX));
    uintptr_t session = read_pointer(owner + LE_SESSION_NATIVE);
    lock_central(central_guard, (void *)(central + LE_MUTEX));
    bool enabled = state[LE_ENABLED] != 0;
    bool busy = state[LE_SCAN_PENDING] || state[LE_SCANNING] ||
                read_pointer(central + LE_SCAN_REQUEST_COUNT);
    unlock_central(central_guard);

    if (!enabled || !in_sdram(session, sizeof(uintptr_t)) || busy) {
        unlock_session(&session_guard);
        lv_label_set_text(status_label, !enabled ? "Turn Bluetooth on in Settings."
                                       : busy ? "Another BLE scan is running. Try again."
                                              : "BLE session is opening. Try again.");
        return;
    }

    /* The stock entry copies its UUID vector. An empty vector selects all
     * services. Keep the other flags from the stock Fitness scan. */
    const uintptr_t all_services[3] = { 0 };
    int start_result = start(central, session, all_services, 0, 1);
    unlock_session(&session_guard);

    static ble_device_t found[DEVICE_LIMIT];
    unsigned count = 0;
    bool saw_scan = false;
    bool read_failed = false;
    if (!start_result) {
        /* Yield the UI task while the stock Bluetooth task receives reports.
         * Keeping this probe within one UI callback lets it stop even if the
         * user presses Home during the scan. No app callback outlives the view. */
        uint32_t started = hb_time_uptime_us();
        for (unsigned i = 0; i < LE_SCAN_US / LE_POLL_US; i++) {
            delay(LE_POLL_US);
            count = poll_ble_devices(central, found, count, &saw_scan, &read_failed);
            if (hb_time_uptime_us() - started >= LE_SCAN_US)
                break;
        }
    }

    lock_session(&session_guard, (void *)(owner + LE_SESSION_MUTEX));
    int stop_result = 0;
    if (read_pointer(owner + LE_SESSION_NATIVE) == session)
        stop_result = stop(central, session);
    unlock_session(&session_guard);

    for (unsigned i = 0; i < count; i++) {
        const ble_device_t *device = &found[i];
        const uint8_t *a = device->address;
        char label[96];
        lv_snprintf(label, sizeof(label), "%s\n%s\n%02X:%02X:%02X:%02X:%02X:%02X %s",
                    device->name,
                    device->type ? device->type : "Unknown type",
                    (unsigned)a[1], (unsigned)a[2], (unsigned)a[3], (unsigned)a[4],
                    (unsigned)a[5], (unsigned)a[6], a[0] ? "Private" : "Public");
        lv_obj_t *row = lv_list_add_button(device_list, NULL, label);
        lv_label_set_long_mode(lv_obj_get_child(row, 0), LV_LABEL_LONG_MODE_WRAP);
        lv_obj_add_event_cb(row, select_ble_device, LV_EVENT_CLICKED, (void *)device);
    }

    /* 401 is the firmware's response when the session has no scan request. */
    if (start_result)
        lv_label_set_text_fmt(status_label, "BLE start error %d", start_result);
    else if (stop_result && stop_result != 401)
        lv_label_set_text_fmt(status_label, "BLE stop error %d", stop_result);
    else if (!saw_scan)
        lv_label_set_text(status_label, "BLE request accepted. Radio did not start.");
    else if (read_failed)
        lv_label_set_text_fmt(status_label, "BLE cache read error.\n%u named devices found.", count);
    else if (count)
        lv_label_set_text_fmt(status_label, "%u named BLE devices.\nTap one to select it.", count);
    else
        lv_label_set_text(status_label, "No named BLE devices found.");
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
    lv_obj_add_state(classic_button, LV_STATE_DISABLED);
    lv_obj_add_state(ble_button, LV_STATE_DISABLED);
    clear_results();
    lv_label_set_text(status_label, "Scanning for BLE devices...\nWait 5 seconds.");
}

/* The stock connection broker serializes both observer calls with its
 * mutex at +0x20. Keep app callbacks installed only while we own the link. */
#define LE_BROKER_TABLE 0x087ea3b0u
#define LE_BROKER_GATT_TABLE 0x087ea428u
#define LE_BROKER_MUTEX 0x20u
#define LE_OBSERVER 0x44u
#define LE_OBSERVER_TABLE (LE_SESSION_VTABLE + 0xc0u)
#define LE_NOTIFY_CALLBACK 0x084b7909u
#define LE_COMPLETE_CALLBACK 0x084b797du
#define LE_DISCOVER_SERVICES 0x084599d5u
#define LE_GET_GATT_BROKER 0x083df995u
#define LE_DISCOVER_CHARACTERISTIC_RANGE 0x082b5f4du
#define LE_GATT_BROKER_TABLE 0x087ea5bcu
#define LE_SUBSCRIBE 0x08459bb1u
#define LE_CHARACTERISTICS_COOKIE 0x0874c324u
#define LE_SUBSCRIBE_COOKIE 0x0874c325u
#define LE_SERVICES_COOKIE 0x0874c326u
#define LE_TEXT_LIMIT 64u

typedef void (*notification_fn)(uintptr_t, uintptr_t, uintptr_t, const uint8_t *, unsigned);
typedef void (*gatt_complete_fn)(uintptr_t, uintptr_t, unsigned, uintptr_t, uintptr_t);
typedef uintptr_t (*gatt_cast_fn)(uintptr_t, uintptr_t, uintptr_t, int);
typedef int (*discover_range_fn)(uintptr_t, uintptr_t, uintptr_t,
                                 unsigned, unsigned, uintptr_t);
typedef int (*subscribe_fn)(uintptr_t, const uintptr_t *, unsigned, unsigned);

static struct {
    uintptr_t owner, broker, session, central, ref;
    bool changed_mode;
    uintptr_t expected_cookie;
    unsigned result;
    bool completed;
    uint16_t service_start, service_end, characteristic_handle, value_handle;
    bool received;
    char message[LE_TEXT_LIMIT + 1];
    uint32_t started;
    uint32_t audio_received;
    ble_audio_state_t audio_state;
    bool playing_audio;
} gatt;
static uintptr_t gatt_observer_storage[10];
#define gatt_observer_table (gatt_observer_storage + 2)

static uint16_t gatt_word(uintptr_t address)
{
    const uint8_t *bytes = (const uint8_t *)address;
    return (uint16_t)(bytes[0] | (bytes[1] << 8));
}

static bool gatt_is_uart_uuid(uintptr_t address, uint8_t member)
{
    uint8_t uuid[16] = {
        0x6e, 0x40, 0x00, member, 0xb5, 0xa3, 0xf3, 0x93,
        0xe0, 0xa9, 0xe5, 0x0e, 0x24, 0xdc, 0xca, 0x9e
    };
    if (gatt_word(address) != sizeof(uuid))
        return false;

    const uint8_t *bytes = (const uint8_t *)(address + 2);
    bool forward = true, reverse = true;
    for (unsigned i = 0; i < sizeof(uuid); i++) {
        forward &= bytes[i] == uuid[i];
        reverse &= bytes[i] == uuid[sizeof(uuid) - 1 - i];
    }
    return forward || reverse;
}

static void gatt_find_uart_attribute(uintptr_t vector, bool service)
{
    if (!in_sdram(vector, 8))
        return;
    uintptr_t first = read_pointer(vector);
    uintptr_t end = read_pointer(vector + 4);
    if (end <= first || (end - first) % 4 || end - first > 4096 ||
        !in_sdram(first, end - first))
        return;
    for (uintptr_t entry = first; entry < end; entry += 4) {
        uintptr_t attribute = read_pointer(entry);
        if (!in_sdram(attribute, 4) || !in_sdram(read_pointer(attribute), 4))
            continue;
        gatt_cast_fn cast = (gatt_cast_fn)0x0842b301u;
        attribute = cast(attribute, 0x0878b3ecu,
                         service ? 0x0878b428u : 0x0878b43cu, -1);
        if (service) {
            if (!in_sdram(attribute, 44) || !gatt_is_uart_uuid(attribute + 26, 1))
                continue;
            gatt.service_start = gatt_word(attribute + 4);
            gatt.service_end = gatt_word(attribute + 24);
            return;
        }
        if (!in_sdram(attribute, 46) || !gatt_is_uart_uuid(attribute + 28, 3) ||
            !(*(const uint8_t *)(attribute + 24) & 0x10u))
            continue;
        gatt.characteristic_handle = gatt_word(attribute + 4);
        gatt.value_handle = gatt_word(attribute + 26);
        return;
    }
}

static void gatt_completed(uintptr_t observer, uintptr_t device, unsigned status,
                           uintptr_t vector, uintptr_t cookie)
{
    if (device == gatt.ref && cookie && cookie == gatt.expected_cookie) {
        if (!status && (cookie == LE_SERVICES_COOKIE || cookie == LE_CHARACTERISTICS_COOKIE))
            gatt_find_uart_attribute(vector, cookie == LE_SERVICES_COOKIE);
        gatt.result = status;
        gatt.completed = true;
    }
    ((gatt_complete_fn)LE_COMPLETE_CALLBACK)(observer, device, status, vector, cookie);
}

static void gatt_notification(uintptr_t observer, uintptr_t device, uintptr_t attribute,
                              const uint8_t *data, unsigned length)
{
    uint16_t handle = in_sdram(attribute, 6) ? gatt_word(attribute + 4) : 0;
    if (device == gatt.ref && handle &&
        (handle == gatt.characteristic_handle || handle == gatt.value_handle) &&
        in_sdram((uintptr_t)data, length) && length && !ble_audio_receive(data, length)) {
        unsigned copy_length = length < LE_TEXT_LIMIT ? length : LE_TEXT_LIMIT;
        for (unsigned i = 0; i < copy_length; i++) {
            uint8_t byte = data[i];
            gatt.message[i] = byte >= 32 && byte <= 126 ? (char)byte : ' ';
        }
        gatt.message[copy_length] = 0;
        gatt.received = true;
    }
    ((notification_fn)LE_NOTIFY_CALLBACK)(observer, device, attribute, data, length);
}

static void gatt_lock(uintptr_t guard[2])
{
    ((firmware_lock_fn)LE_CENTRAL_LOCK)(guard, (void *)(gatt.broker + LE_BROKER_MUTEX));
}

static void gatt_unlock(uintptr_t guard[2])
{
    ((firmware_unlock_fn)LE_CENTRAL_UNLOCK)(guard);
}

static void drop_ble_link(uintptr_t owner, uintptr_t session, uintptr_t device,
                          bool changed_mode)
{
    uintptr_t guard = 0;
    ((firmware_lock_fn)BT_CLIENT_LOCK)(&guard, (void *)(owner + LE_SESSION_MUTEX));
    if (read_pointer(owner + LE_SESSION_NATIVE) == session) {
        uintptr_t broker = ((get_ble_broker_fn)LE_GET_BROKER)();
        ((disconnect_ble_fn)LE_DISCONNECT)(broker, session, device);
    }
    ((firmware_unlock_fn)BT_CLIENT_UNLOCK)(&guard);
    if (changed_mode && read_pointer(owner + LE_SESSION_NATIVE) == session &&
        ((get_ble_mode_fn)LE_GET_MODE)(owner) == LE_MODE_ACTIVE)
        ((set_ble_mode_fn)LE_SET_MODE)(owner, LE_MODE_IDLE);
}

static int finish_gatt(void)
{
    if (gatt.ref) {
        uintptr_t guard[2] = {0};
        gatt_lock(guard);
        uintptr_t observer = gatt.owner + LE_OBSERVER;
        if (read_pointer(observer) == (uintptr_t)gatt_observer_table)
            *(volatile uintptr_t *)observer = LE_OBSERVER_TABLE;
        gatt_unlock(guard);
        drop_ble_link(gatt.owner, gatt.session, gatt.ref, gatt.changed_mode);
        ((release_ble_device_fn)LE_RELEASE_DEVICE)(&gatt.ref);
        memset(&gatt, 0, sizeof(gatt));
        ble_audio_reset();
    }
    return 1;
}

static int prepare_unload(void)
{
    finish_gatt();
    return ble_audio_release();
}

static const char *start_gatt(uintptr_t owner, uintptr_t session, uintptr_t central,
                       uintptr_t ref, bool changed_mode)
{
    uintptr_t broker = ((get_ble_broker_fn)LE_GET_BROKER)();
    if (!hb_shared)
        return "BLE link opened. App runtime unavailable.";
    if (!in_sdram(broker, 0x60) ||
        read_pointer(broker) != LE_BROKER_TABLE ||
        read_pointer(broker + 0x10) != LE_BROKER_GATT_TABLE ||
        read_pointer(LE_BROKER_GATT_TABLE) != 0x084b5359u ||
        read_pointer(LE_BROKER_GATT_TABLE + 4) != 0x084b5041u)
        return "BLE link opened. Broker table mismatch.";
    if (read_pointer(LE_SESSION_VTABLE + 0x6c) != LE_DISCOVER_SERVICES ||
        read_pointer(LE_SESSION_VTABLE + 0x70) != LE_SUBSCRIBE ||
        read_pointer(LE_OBSERVER_TABLE + 0x18) != LE_NOTIFY_CALLBACK ||
        read_pointer(LE_OBSERVER_TABLE + 0x1c) != LE_COMPLETE_CALLBACK)
        return "BLE link opened. GATT session table mismatch.";

    gatt.broker = broker;
    uintptr_t guard[2] = {0};
    gatt_lock(guard);
    uintptr_t observer = owner + LE_OBSERVER;
    bool available = read_pointer(observer) == LE_OBSERVER_TABLE &&
                     read_pointer(owner + LE_SESSION_NATIVE) == session;
    if (available) {
        memcpy(gatt_observer_storage, (const void *)(LE_OBSERVER_TABLE - 8),
               sizeof(gatt_observer_storage));
        gatt_observer_table[6] = (uintptr_t)gatt_notification;
        gatt_observer_table[7] = (uintptr_t)gatt_completed;
        gatt.owner = owner;
        gatt.session = session;
        gatt.central = central;
        gatt.ref = ref;
        gatt.changed_mode = changed_mode;
        gatt.expected_cookie = LE_SERVICES_COOKIE;
        gatt.started = lv_tick_get();
        *(volatile uintptr_t *)observer = (uintptr_t)gatt_observer_table;
    }
    gatt_unlock(guard);
    return available ? NULL : "BLE link opened. GATT session changed.";
}

static void end_notification_test(const char *status)
{
    finish_gatt();
    lv_label_set_text(connect_label, "Connect");
    lv_label_set_text(status_label, status);
    stop_polling();
}

static void poll_gatt(lv_timer_t *timer)
{
    (void)timer;
    if (read_pointer(gatt.owner + LE_SESSION_NATIVE) != gatt.session ||
        !((ble_link_state_fn)LE_LINK_CONNECTED)(gatt.central, gatt.ref)) {
        end_notification_test("BLE link closed.");
        return;
    }

    uintptr_t guard[2] = {0};
    char message[LE_TEXT_LIMIT + 1];
    gatt_lock(guard);
    uintptr_t phase = gatt.expected_cookie;
    unsigned result = gatt.result;
    bool completed = gatt.completed;
    bool received = gatt.received;
    uint16_t characteristic = gatt.characteristic_handle;
    uint16_t service_start = gatt.service_start;
    uint16_t service_end = gatt.service_end;
    ble_audio_progress_t audio = ble_audio_progress();
    if (received) {
        memcpy(message, gatt.message, sizeof(message));
        gatt.received = false;
    }
    gatt_unlock(guard);

    if (!phase && (received || audio.received != gatt.audio_received ||
                   audio.state != gatt.audio_state || ble_audio_playing()))
        gatt.started = lv_tick_get();
    if (lv_tick_elaps(gatt.started) >= (phase ? 12000u : 60000u)) {
        end_notification_test(phase ? "GATT setup timed out." : "BLE listening timed out.");
        return;
    }
    if (phase && completed) {
        if (result) {
            const char *operation = phase == LE_SERVICES_COOKIE ? "Service discovery" :
                                    phase == LE_CHARACTERISTICS_COOKIE ? "UART TX discovery" :
                                    "UART TX subscribe";
            end_notification_test(operation);
            lv_label_set_text_fmt(status_label, "%s error %u", operation, result);
            return;
        }
        uintptr_t next = phase == LE_SERVICES_COOKIE ? LE_CHARACTERISTICS_COOKIE :
                         phase == LE_CHARACTERISTICS_COOKIE ? LE_SUBSCRIBE_COOKIE : 0;
        if (next == LE_SUBSCRIBE_COOKIE && !characteristic) {
            end_notification_test("UART TX Notify characteristic missing.");
            return;
        }

        gatt_lock(guard);
        gatt.expected_cookie = next;
        gatt.completed = false;
        gatt_unlock(guard);
        gatt.started = lv_tick_get();
        if (next == LE_CHARACTERISTICS_COOKIE) {
            lv_label_set_text(status_label, "Finding UART TX...");
            if (!service_start || service_end <= service_start) {
                end_notification_test("Nordic UART service missing.");
                return;
            }
            uintptr_t broker = ((get_ble_broker_fn)LE_GET_GATT_BROKER)();
            if (read_pointer(broker) != LE_GATT_BROKER_TABLE) {
                end_notification_test("GATT command broker mismatch.");
                return;
            }
            discover_range_fn discover = (discover_range_fn)LE_DISCOVER_CHARACTERISTIC_RANGE;
            if (discover(broker, gatt.session, gatt.ref, service_start, service_end,
                         LE_CHARACTERISTICS_COOKIE))
                end_notification_test("UART TX discovery rejected.");
        } else if (next == LE_SUBSCRIBE_COOKIE) {
            lv_label_set_text(status_label, "Subscribing to UART TX...");
            /* The firmware looks up the cached characteristic by its handle
             * and discovers its client configuration descriptor itself. */
            if (!((subscribe_fn)LE_SUBSCRIBE)(gatt.owner, &gatt.ref, characteristic, 1))
                end_notification_test("Notification subscription rejected.");
        } else {
            lv_label_set_text(status_label, "Subscribed. Send text or audio.");
        }
    }

    if (!gatt.ref || gatt.expected_cookie)
        return;

    bool audio_changed = audio.state != gatt.audio_state || audio.received != gatt.audio_received;
    gatt.audio_state = audio.state;
    gatt.audio_received = audio.received;
    if (audio.state == BLE_AUDIO_READY) {
        if (ble_audio_playing()) {
            lv_label_set_text(status_label, "Clip received. Waiting to play...");
            return;
        }
        gatt.playing_audio = ble_audio_play();
        lv_label_set_text(status_label, gatt.playing_audio
                         ? "Playing received audio." : "Could not play received audio.");
    } else if (audio_changed && audio.state == BLE_AUDIO_RECEIVING) {
        lv_label_set_text_fmt(status_label, "Receiving audio... %u%%",
                              (unsigned)(audio.received * 100 / audio.total));
    } else if (audio.state == BLE_AUDIO_ERROR) {
        lv_label_set_text(status_label, "Audio incomplete or invalid. Try again.");
    } else if (audio.state == BLE_AUDIO_CANCELLED) {
        lv_label_set_text(status_label, "Audio transfer cancelled.");
    } else if (gatt.playing_audio && !ble_audio_playing()) {
        gatt.playing_audio = false;
        ble_audio_release();
        if (audio.state == BLE_AUDIO_IDLE)
            lv_label_set_text(status_label, "Audio finished.");
    } else if (received && audio.state == BLE_AUDIO_IDLE) {
        lv_label_set_text_fmt(status_label, "Received:\n%s", message);
    }

    if (audio.state == BLE_AUDIO_READY || audio.state == BLE_AUDIO_ERROR ||
        audio.state == BLE_AUDIO_CANCELLED) {
        gatt_lock(guard);
        /* A new start packet may have arrived since an error or cancellation. */
        if (ble_audio_progress().state == audio.state)
            ble_audio_reset();
        gatt_unlock(guard);
    }
}

static const char *test_ble_connection(const ble_device_t *target)
{
    const volatile uint16_t *connect_code = (const volatile uint16_t *)(LE_CONNECT & ~1u);
    const volatile uint16_t *disconnect_code = (const volatile uint16_t *)(LE_DISCONNECT & ~1u);
    if (!firmware_matches() || connect_code[0] != 0xe92d || connect_code[1] != 0x41f0 ||
        disconnect_code[0] != 0xb5f7)
        return "BLE connection firmware mismatch.";

    uintptr_t central = read_pointer(LE_CENTRAL_SLOT);
    uintptr_t owner = read_pointer(LE_SESSION_SLOT);
    uintptr_t manager = read_pointer(LE_DEVICE_MANAGER_SLOT);
    if (!in_sdram(central, LE_CACHE_COUNT + sizeof(uintptr_t)) ||
        read_pointer(central) != LE_CENTRAL_VTABLE || !in_sdram(owner, 0xccu) ||
        read_pointer(owner) != LE_SESSION_VTABLE || !in_sdram(manager, 0x64u) ||
        read_pointer(manager) != LE_DEVICE_MANAGER_VTABLE)
        return "BLE session is unavailable. Scan again.";

    uintptr_t session = read_pointer(owner + LE_SESSION_NATIVE);
    if (!in_sdram(session, sizeof(uintptr_t)))
        return "BLE session is unavailable. Scan again.";

    uintptr_t ready_method = read_pointer(LE_SESSION_VTABLE + LE_READY_SLOT);
    if (read_pointer(LE_SESSION_VTABLE + LE_SET_MODE_SLOT) != LE_SET_MODE ||
        read_pointer(LE_SESSION_VTABLE + LE_GET_MODE_SLOT) != LE_GET_MODE ||
        !(ready_method & 1u) || ready_method < SDRAM_START ||
        ready_method >= 0x0867f8e4u)
        return "BLE session method mismatch.";

    const volatile uint8_t *state = (const volatile uint8_t *)central;
    if (!state[LE_ENABLED])
        return "Turn Bluetooth on in Settings.";
    if (!read_pointer(owner + LE_SESSION_HANDLE) ||
        !read_pointer(owner + LE_SESSION_LOCAL_DEVICE))
        return "BLE session is opening. Try again.";

    find_ble_device_fn find = (find_ble_device_fn)LE_FIND_DEVICE;
    uintptr_t device = find(manager, target->address, 0);
    if (!in_sdram(device, LE_DEVICE_ADDRESS + LE_ADDRESS_SIZE) ||
        read_pointer(device) != LE_DEVICE_VTABLE)
        return "Device unavailable. Scan again.";

    retain_ble_device_fn retain = (retain_ble_device_fn)LE_RETAIN_DEVICE;
    release_ble_device_fn release = (release_ble_device_fn)LE_RELEASE_DEVICE;
    connect_ble_fn connect = (connect_ble_fn)LE_CONNECT;
    ble_link_state_fn connected = (ble_link_state_fn)LE_LINK_CONNECTED;
    ble_link_state_fn pending = (ble_link_state_fn)LE_LINK_PENDING;
    delay_us_fn delay = (delay_us_fn)LE_DELAY_US;
    uintptr_t ref = 0;
    retain(&ref, device);

    const char *status;
    if (connected(central, device)) {
        status = "Device is already connected.";
    } else if (pending(central, device)) {
        status = "Device already has a pending connection.";
    } else {
        get_ble_mode_fn get_mode = (get_ble_mode_fn)LE_GET_MODE;
        get_ble_mode_fn ready = (get_ble_mode_fn)ready_method;
        set_ble_mode_fn set_mode = (set_ble_mode_fn)LE_SET_MODE;
        unsigned original_mode = get_mode(owner);
        bool changed_mode = original_mode == LE_MODE_IDLE;

        /* Fitness enters mode 3 before connecting and restores mode 2 on exit. */
        if (changed_mode)
            set_mode(owner, LE_MODE_ACTIVE);

        bool can_connect = ready(owner) != 0;
        for (unsigned i = 0; !can_connect && i < LE_READY_POLLS; i++) {
            delay(LE_POLL_US);
            if (read_pointer(owner + LE_SESSION_NATIVE) != session)
                break;
            can_connect = ready(owner) != 0;
        }

        if (!can_connect) {
            status = "iPod BLE session did not become ready.";
        } else if (!connect(owner, &ref)) {
            status = "iPod rejected the BLE request locally.";
        } else {
            bool linked = false;
            for (unsigned i = 0; i < LE_CONNECT_POLLS; i++) {
                delay(LE_POLL_US);
                linked = connected(central, device) != 0;
                if (linked || read_pointer(owner + LE_SESSION_NATIVE) != session)
                    break;
            }
            if (linked) {
                status = start_gatt(owner, session, central, ref, changed_mode);
                if (!status)
                    return NULL;
            } else {
                status = "No BLE link. Keep the phone advertising.";
            }
            drop_ble_link(owner, session, device, false);
        }

        if (changed_mode && read_pointer(owner + LE_SESSION_NATIVE) == session &&
            get_mode(owner) == LE_MODE_ACTIVE)
            set_mode(owner, LE_MODE_IDLE);
    }

    release(&ref);
    return status;
}

static void finish_connect_test(lv_timer_t *timer)
{
    const char *status = test_ble_connection(selected_device);
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
    if (!((connect_ble_fn)LE_DISCOVER_SERVICES)(gatt.owner, &gatt.ref))
        end_notification_test("Service discovery rejected.");
}

static void begin_connect_test(lv_event_t *event)
{
    (void)event;
    if (gatt.ref) {
        end_notification_test("BLE disconnected.");
        return;
    }
    if (scan_timer || !selected_device)
        return;

    scan_timer = lv_timer_create(finish_connect_test, 100, NULL);
    if (!scan_timer) {
        lv_label_set_text(status_label, "Could not start BLE connection test.");
        return;
    }
    lv_obj_add_state(classic_button, LV_STATE_DISABLED);
    lv_obj_add_state(ble_button, LV_STATE_DISABLED);
    lv_obj_add_state(connect_button, LV_STATE_DISABLED);
    lv_label_set_text_fmt(status_label, "Connecting to %s...", selected_device->name);
}

HB_APP_ENTRY(payload_entry)
{
    lv_obj_t *screen = lv_screen_active();

    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "Bluetooth Test");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 40);

    status_label = lv_label_create(screen);
    lv_obj_set_width(status_label, 208);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(status_label, "Choose a Bluetooth test.");
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

    device_list = lv_list_create(screen);
    lv_obj_set_size(device_list, 208, 140);
    lv_obj_align(device_list, LV_ALIGN_TOP_MID, 0, 211);

    connect_button = lv_button_create(screen);
    lv_obj_set_size(connect_button, 208, 40);
    lv_obj_align(connect_button, LV_ALIGN_TOP_MID, 0, 360);
    lv_obj_add_state(connect_button, LV_STATE_DISABLED);
    lv_obj_add_event_cb(connect_button, begin_connect_test, LV_EVENT_CLICKED, NULL);

    connect_label = lv_label_create(connect_button);
    lv_label_set_text(connect_label, "Connect");
    lv_obj_center(connect_label);
}
