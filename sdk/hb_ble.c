#include "hb_ble.h"
#include "hb_bt_internal.h"
#include "hb_sdk.h"
#include "hb_heap.h"
#include "hb_pcm.h"
#include "hb_ble_audio.h"
#include "hb_shared.h"
#include "lvgl/lvgl.h"
#include <string.h>

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
#define LE_REPORT_VTABLE 0x087ebc98u
#define LE_REPORT_NAME 0x0cu
#define LE_REPORT_SERVICES 0x38u
#define LE_REPORT_SLOTS 3u
#define LE_UUID_SIZE 18u
#define LE_SERVICE_LIMIT 16u

typedef void (*firmware_lock_fn)(uintptr_t *guard, void *mutex);
typedef void (*firmware_unlock_fn)(uintptr_t *guard);
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

#define LE_ADDRESS_SIZE HB_BLE_ADDRESS_SIZE
#define LE_NAME_SIZE HB_BLE_NAME_SIZE

static char scan_error[48];

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

static void read_ble_report(uintptr_t report, hb_ble_device_t *result)
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
static bool read_ble_devices(uintptr_t central, hb_ble_device_t devices[HB_BLE_SCAN_LIMIT],
                             unsigned limit, unsigned *count, hb_ble_filter_fn filter)
{
    uintptr_t sentinel = read_pointer(central + LE_CACHE_SENTINEL);
    if (sentinel % sizeof(uintptr_t) ||
        !in_sdram(sentinel, LE_TREE_ROOT + sizeof(uintptr_t)))
        return false;

    uintptr_t stack[32];
    unsigned depth = 0;
    uintptr_t node = read_pointer(sentinel + LE_TREE_ROOT);
    for (unsigned visited = 0; visited < limit && *count < HB_BLE_SCAN_LIMIT; visited++) {
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

        hb_ble_device_t *result = &devices[*count];
        *result = (hb_ble_device_t){ 0 };
        const volatile uint8_t *address = (const volatile uint8_t *)(device + LE_DEVICE_ADDRESS);
        for (unsigned i = 0; i < LE_ADDRESS_SIZE; i++)
            result->address[i] = address[i];

        uintptr_t reports = read_pointer(node + LE_NODE_REPORTS);
        if (in_sdram(reports, LE_REPORT_SLOTS * sizeof(uintptr_t))) {
            for (unsigned i = 0; i < LE_REPORT_SLOTS; i++)
                read_ble_report(read_pointer(reports + i * sizeof(uintptr_t)), result);
        }
        if (result->name[0] && (!filter || filter(result)))
            (*count)++;
        node = read_pointer(node + LE_NODE_RIGHT);
    }
    return true;
}

static unsigned poll_ble_devices(uintptr_t central, hb_ble_device_t found[HB_BLE_SCAN_LIMIT],
                                 unsigned count, bool *saw_scan, bool *read_failed,
                                 hb_ble_filter_fn filter)
{
    firmware_lock_fn lock = (firmware_lock_fn)LE_CENTRAL_LOCK;
    firmware_unlock_fn unlock = (firmware_unlock_fn)LE_CENTRAL_UNLOCK;
    uintptr_t guard[2] = { 0 };
    static hb_ble_device_t snapshot[HB_BLE_SCAN_LIMIT];
    unsigned copied = 0;

    lock(guard, (void *)(central + LE_MUTEX));
    const volatile uint8_t *state = (const volatile uint8_t *)central;
    if (state[LE_SCANNING]) {
        *saw_scan = true;
        unsigned total = read_pointer(central + LE_CACHE_COUNT);
        unsigned limit = total < LE_CACHE_READ_LIMIT ? total : LE_CACHE_READ_LIMIT;
        if (limit && !read_ble_devices(central, snapshot, limit, &copied, filter)) {
            *read_failed = true;
            copied = 0;
        }
    }
    unlock(guard);

    for (unsigned i = 0; i < copied; i++) {
        const hb_ble_device_t *device = &snapshot[i];
        unsigned j = 0;
        while (j < count && memcmp(device->address, found[j].address, LE_ADDRESS_SIZE))
            j++;
        if (j == count) {
            if (count < HB_BLE_SCAN_LIMIT)
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

const char *hb_ble_scan(hb_ble_device_t found[HB_BLE_SCAN_LIMIT],
                        unsigned *found_count, hb_ble_filter_fn filter)
{
    if (!found_count)
        return "Provide a BLE scan result count.";
    *found_count = 0;
    if (!found)
        return "Provide a BLE scan result buffer.";
    const volatile uint16_t *handler = (const volatile uint16_t *)LE_REPORT_HANDLER;
    const volatile uint16_t *start_code = (const volatile uint16_t *)(LE_START_SCAN & ~1u);
    const volatile uint16_t *stop_code = (const volatile uint16_t *)(LE_STOP_SCAN & ~1u);
    if (!firmware_matches() || handler[0] != 0xe92d || handler[1] != 0x4fff ||
        handler[2] != 0x4606 || handler[3] != 0xb095 ||
        start_code[0] != 0xe92d || start_code[1] != 0x43ff ||
        stop_code[0] != 0xe92d || stop_code[1] != 0x41f3) {
        return "BLE firmware signature mismatch.";
    }

    uintptr_t central = read_pointer(LE_CENTRAL_SLOT);
    if (!central) {
        return "Turn Bluetooth on in Settings first.";
    }
    if (!in_sdram(central, LE_CACHE_COUNT + sizeof(uintptr_t)) ||
        read_pointer(central) != LE_CENTRAL_VTABLE) {
        return "BLE controller table mismatch.";
    }

    const volatile uint8_t *state = (const volatile uint8_t *)central;
    if (!state[LE_ENABLED]) {
        return "Turn Bluetooth on in Settings.";
    }

    uintptr_t owner = read_pointer(LE_SESSION_SLOT);
    if (!owner) {
        get_ble_session_fn get_session = (get_ble_session_fn)LE_GET_SESSION;
        owner = get_session();
    }
    if (!in_sdram(owner, 0xccu) || read_pointer(owner) != LE_SESSION_VTABLE) {
        return "BLE session is unavailable.";
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
        return !enabled ? "Turn Bluetooth on in Settings."
                        : busy ? "Another BLE scan is running. Try again."
                               : "BLE session is opening. Try again.";
    }

    /* The stock entry copies its UUID vector. An empty vector selects all
     * services. Keep the other flags from the stock Fitness scan. */
    const uintptr_t all_services[3] = { 0 };
    int start_result = start(central, session, all_services, 0, 1);
    unlock_session(&session_guard);

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
            count = poll_ble_devices(central, found, count, &saw_scan, &read_failed, filter);
            if (hb_time_uptime_us() - started >= LE_SCAN_US)
                break;
        }
    }

    lock_session(&session_guard, (void *)(owner + LE_SESSION_MUTEX));
    int stop_result = 0;
    if (!start_result && read_pointer(owner + LE_SESSION_NATIVE) == session)
        stop_result = stop(central, session);
    unlock_session(&session_guard);

    *found_count = count;
    if (start_result || (stop_result && stop_result != 401)) {
        lv_snprintf(scan_error, sizeof(scan_error), "BLE %s error %d",
                    start_result ? "start" : "stop", start_result ? start_result : stop_result);
        return scan_error;
    }
    if (!saw_scan)
        return "BLE request accepted. Radio did not start.";
    return read_failed ? "Could not read scan results. Try again." : NULL;
}

#ifdef HB_BLE_UART
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
#define LE_READ_COOKIE 0x0874c336u
#define LE_READ_TASK_TABLE 0x087ec3d8u
#define LE_READ_TASK_DESTROY 0x08431f85u
#define LE_ATT_READ 0x080e9f0du
#define LE_ATT_WRITE 0x080ea60du
#define LE_READ_RECORD_LIMIT 21u

typedef void (*notification_fn)(uintptr_t, uintptr_t, uintptr_t, const uint8_t *, unsigned);
typedef void (*gatt_complete_fn)(uintptr_t, uintptr_t, unsigned, uintptr_t, uintptr_t);
typedef uintptr_t (*gatt_cast_fn)(uintptr_t, uintptr_t, uintptr_t, int);
typedef int (*discover_range_fn)(uintptr_t, uintptr_t, uintptr_t,
                                 unsigned, unsigned, uintptr_t);
typedef int (*subscribe_fn)(uintptr_t, const uintptr_t *, unsigned, unsigned);

static struct {
    uintptr_t owner, broker, session, central, ref;
    bool changed_mode, receive_audio, chat;
    uintptr_t expected_cookie;
    unsigned result;
    bool completed;
    uint16_t service_start, service_end, characteristic_handle, value_handle;
    uint16_t audio_read_handle;
    uint16_t chat_handle;
    bool chat_pending;
    uint32_t chat_started;
    uint32_t read_started;
    bool read_pending, read_idle, read_ready;
    bool received;
    uint8_t message[HB_BLE_NOTIFICATION_LIMIT];
    unsigned message_length;
    uint32_t started;
    uint32_t audio_received;
    ble_audio_state_t audio_state;
    bool playing_audio;
} gatt;
static uintptr_t gatt_observer_storage[10];
#define gatt_observer_table (gatt_observer_storage + 2)

/* The broker owns the task until it destroys it. Only that task writes the
 * mailbox, and the UI consumes it after the release store in its destructor. */
static struct {
    unsigned status, length;
    uint8_t data[LE_READ_RECORD_LIMIT];
    bool active;
} audio_read;
static uintptr_t read_task_storage[5];
/* ATT can retain the payload until the broker destroys the task. */
static char chat_message[HB_BLE_CHAT_LIMIT];
static unsigned chat_length;
static uint32_t unload_started;
static bool read_used, unload_waiting;

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
        if (!in_sdram(attribute, 46))
            continue;
        uint8_t properties = *(const uint8_t *)(attribute + 24);
        if ((properties & 0x10u) && gatt_is_uart_uuid(attribute + 28, 3)) {
            gatt.characteristic_handle = gatt_word(attribute + 4);
            gatt.value_handle = gatt_word(attribute + 26);
        } else if (gatt.receive_audio && (properties & 0x02u) &&
                   gatt_is_uart_uuid(attribute + 28, 4)) {
            gatt.audio_read_handle = gatt_word(attribute + 26);
        } else if (gatt.chat && (properties & 0x08u) &&
                   gatt_is_uart_uuid(attribute + 28, 2)) {
            gatt.chat_handle = gatt_word(attribute + 26);
        }
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
        in_sdram((uintptr_t)data, length) && length &&
        (!gatt.receive_audio || !ble_audio_receive(data, length))) {
        gatt.message_length = length < sizeof(gatt.message) ? length : sizeof(gatt.message);
        memcpy(gatt.message, data, gatt.message_length);
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

static bool read_task_active(void)
{
    return __atomic_load_n(&audio_read.active, __ATOMIC_ACQUIRE);
}

static void destroy_audio_read(uintptr_t task)
{
    ((void (*)(uintptr_t))LE_READ_TASK_DESTROY)(task);
    __atomic_store_n(&audio_read.active, false, __ATOMIC_RELEASE);
}

static void complete_audio_read(unsigned status, uintptr_t att, const uint8_t *data,
                                unsigned length, uintptr_t task)
{
    (void)att;
    if (!status && (!length || length > sizeof(audio_read.data) ||
                    !in_sdram((uintptr_t)data, length)))
        status = 1;
    audio_read.status = status;
    if (!status) {
        memcpy(audio_read.data, data, length);
        audio_read.length = length;
    }

    /* Complete the broker command without inserting an attribute in its cache. */
    uintptr_t attributes[3] = {0};
    ((void (*)(uintptr_t, unsigned, uintptr_t *))0x083ec2a1u)(task, status, attributes);
}

static int execute_audio_read(uintptr_t task)
{
    typedef void (*read_callback_fn)(unsigned, uintptr_t, const uint8_t *, unsigned, uintptr_t);
    typedef int (*att_read_fn)(uintptr_t, read_callback_fn, unsigned, uintptr_t);
    uintptr_t guard = 0;
    ((firmware_unlock_fn)0x083f0811u)(&guard);
    int error = ((att_read_fn)LE_ATT_READ)(read_pointer(task + 12),
                                         complete_audio_read, gatt_word(task + 20), task);
    ((firmware_unlock_fn)0x083f07fdu)(&guard);
    ((firmware_unlock_fn)0x083f07f1u)(&guard);
    return error != 0;
}

static void complete_chat_write(unsigned status, uintptr_t att, uintptr_t task)
{
    (void)att;
    audio_read.status = status;
    uintptr_t attributes[3] = {0};
    ((void (*)(uintptr_t, unsigned, uintptr_t *))0x083ec2a1u)(task, status, attributes);
}

static int execute_chat_write(uintptr_t task)
{
    typedef void (*write_callback_fn)(unsigned, uintptr_t, uintptr_t);
    typedef int (*att_write_fn)(uintptr_t, write_callback_fn, unsigned, const char *, unsigned, uintptr_t);
    uintptr_t guard = 0;
    ((firmware_unlock_fn)0x083f0811u)(&guard);
    int error = ((att_write_fn)LE_ATT_WRITE)(read_pointer(task + 12), complete_chat_write,
                                            gatt_word(task + 20), chat_message, chat_length, task);
    if (error)
        audio_read.status = (unsigned)error;
    ((firmware_unlock_fn)0x083f07fdu)(&guard);
    ((firmware_unlock_fn)0x083f07f1u)(&guard);
    return error != 0;
}

/* Reads and writes share the broker command slot. Use the stock command
 * constructor and destructor, with an execute method for the chosen ATT call. */
static bool start_att_task(uint16_t handle, int (*execute)(uintptr_t))
{
    uintptr_t broker = ((get_ble_broker_fn)LE_GET_GATT_BROKER)();
    if (!in_sdram(broker, 0x64) || read_pointer(broker) != LE_GATT_BROKER_TABLE ||
        read_pointer(LE_READ_TASK_TABLE + 4) != LE_READ_TASK_DESTROY ||
        read_pointer(LE_READ_TASK_TABLE + 8) != 0x08431db5u)
        return false;

    uintptr_t task = (uintptr_t)hb_os_alloc(24);
    if (!task)
        return false;
    typedef void (*task_init_fn)(uintptr_t, uintptr_t, uintptr_t, unsigned);
    ((task_init_fn)0x083d343du)(task, broker + 12, gatt.ref, 0);
    memcpy(read_task_storage, (const void *)(LE_READ_TASK_TABLE - 8), sizeof(read_task_storage));
    read_task_storage[3] = (uintptr_t)destroy_audio_read;
    read_task_storage[4] = (uintptr_t)execute;
    *(uintptr_t *)task = (uintptr_t)(read_task_storage + 2);
    *(uint16_t *)(task + 20) = handle;

    uintptr_t request[3] = {task, gatt.session, LE_READ_COOKIE};
    audio_read.status = 1;
    audio_read.length = 0;
    __atomic_store_n(&audio_read.active, true, __ATOMIC_RELEASE);
    if (((int (*)(uintptr_t, uintptr_t, uintptr_t *))0x083b5e75u)(broker, gatt.ref, request)) {
        if (request[0])
            destroy_audio_read(request[0]);
        return false;
    }
    ((void (*)(uintptr_t, uintptr_t))0x083b5f75u)(broker, gatt.ref);
    return true;
}

/* Called with the connection broker lock held, like the notification receiver. */
static bool consume_audio_read(void)
{
    if (!gatt.read_pending || read_task_active())
        return false;
    gatt.read_pending = false;
    if (audio_read.status || audio_read.length < 2 || audio_read.data[0] != 0) {
        gatt.audio_read_handle = 0;
        return true;
    }
    gatt.read_idle = audio_read.length == 2 && audio_read.data[1] == 'I';
    if (!gatt.read_idle)
        ble_audio_receive(audio_read.data, audio_read.length);
    return false;
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

static void finish_gatt(void)
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
}
static const char *start_gatt(uintptr_t owner, uintptr_t session, uintptr_t central,
                              uintptr_t ref, bool changed_mode, bool receive_audio, bool chat)
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
        gatt.receive_audio = receive_audio;
        gatt.chat = chat;
        gatt.expected_cookie = LE_SERVICES_COOKIE;
        gatt.started = lv_tick_get();
        *(volatile uintptr_t *)observer = (uintptr_t)gatt_observer_table;
    }
    gatt_unlock(guard);
    return available ? NULL : "BLE link opened. GATT session changed.";
}

void hb_ble_poll(hb_ble_update_t *update)
{
    *update = (hb_ble_update_t){ .poll_ms = 100 };
    if (!gatt.ref)
        return;
    if (read_pointer(gatt.owner + LE_SESSION_NATIVE) != gatt.session ||
        !((ble_link_state_fn)LE_LINK_CONNECTED)(gatt.central, gatt.ref)) {
        finish_gatt();
        update->status = "BLE link closed.";
        return;
    }

    uintptr_t guard[2] = {0};
    gatt_lock(guard);
    bool read_failed = consume_audio_read();
    uintptr_t phase = gatt.expected_cookie;
    unsigned result = gatt.result;
    bool completed = gatt.completed;
    bool received = gatt.received;
    uint16_t characteristic = gatt.characteristic_handle;
    uint16_t service_start = gatt.service_start;
    uint16_t service_end = gatt.service_end;
    ble_audio_progress_t audio = ble_audio_progress();
    if (received) {
        update->length = gatt.message_length;
        memcpy(update->data, gatt.message, update->length);
        gatt.received = false;
    }
    gatt_unlock(guard);

    if (!phase && (received || audio.received != gatt.audio_received ||
                   audio.state != gatt.audio_state || hb_pcm_playing()))
        gatt.started = lv_tick_get();
    if ((phase || !gatt.chat) && lv_tick_elaps(gatt.started) >= (phase ? 12000u : 60000u)) {
        finish_gatt();
        update->status = phase ? "GATT setup timed out." : "BLE listening timed out.";
        return;
    }
    if (phase && completed) {
        if (result) {
            const char *operation = phase == LE_SERVICES_COOKIE ? "Service discovery" :
                                    phase == LE_CHARACTERISTICS_COOKIE ? "UART TX discovery" :
                                    "UART TX subscribe";
            finish_gatt();
            lv_snprintf(update->text, sizeof(update->text), "%s error %u", operation, result);
            update->status = update->text;
            return;
        }
        uintptr_t next = phase == LE_SERVICES_COOKIE ? LE_CHARACTERISTICS_COOKIE :
                         phase == LE_CHARACTERISTICS_COOKIE ? LE_SUBSCRIBE_COOKIE : 0;
        if (next == LE_SUBSCRIBE_COOKIE && !characteristic) {
            finish_gatt();
            update->status = "UART TX Notify characteristic missing.";
            return;
        }

        gatt_lock(guard);
        gatt.expected_cookie = next;
        gatt.completed = false;
        gatt_unlock(guard);
        gatt.started = lv_tick_get();
        if (next == LE_CHARACTERISTICS_COOKIE) {
            update->status = "Finding UART TX...";
            if (!service_start || service_end <= service_start) {
                finish_gatt();
                update->status = "Nordic UART service missing.";
                return;
            }
            uintptr_t broker = ((get_ble_broker_fn)LE_GET_GATT_BROKER)();
            if (read_pointer(broker) != LE_GATT_BROKER_TABLE) {
                finish_gatt();
                update->status = "GATT command broker mismatch.";
                return;
            }
            discover_range_fn discover = (discover_range_fn)LE_DISCOVER_CHARACTERISTIC_RANGE;
            if (discover(broker, gatt.session, gatt.ref, service_start, service_end,
                         LE_CHARACTERISTICS_COOKIE)) {
                finish_gatt();
                update->status = "UART TX discovery rejected.";
            }
        } else if (next == LE_SUBSCRIBE_COOKIE) {
            update->status = "Subscribing to UART TX...";
            /* The firmware looks up the cached characteristic by its handle
             * and discovers its client configuration descriptor itself. */
            if (!((subscribe_fn)LE_SUBSCRIBE)(gatt.owner, &gatt.ref, characteristic, 1)) {
                finish_gatt();
                update->status = "Notification subscription rejected.";
            }
        } else {
            update->status = gatt.chat ? (gatt.chat_handle ? "Connected." : "Connected. Update the Mac app to send.") :
                             gatt.audio_read_handle ? "Subscribed. Checking GATT read..." :
                             gatt.receive_audio ? "Subscribed. Send text or audio." :
                                                  "Subscribed to UART TX.";
            if (gatt.audio_read_handle)
                update->poll_ms = 25;
        }
    }

    if (!gatt.ref || gatt.expected_cookie)
        return;
    if (gatt.chat_pending) {
        if (!read_task_active()) {
            gatt.chat_pending = false;
            if (audio_read.status) {
                lv_snprintf(update->text, sizeof(update->text), "BLE write failed (%u).", audio_read.status);
                update->status = update->text;
            } else {
                update->status = "Sent to Mac.";
            }
        } else if (lv_tick_elaps(gatt.chat_started) >= 12000u) {
            finish_gatt();
            update->status = "BLE write timed out. Reconnect to retry.";
        }
    }
    if (!gatt.receive_audio)
        return;
    if (gatt.audio_read_handle || gatt.playing_audio)
        update->poll_ms = 25;

    if (gatt.read_pending && lv_tick_elaps(gatt.read_started) >= 12000u) {
        finish_gatt();
        update->status = "GATT audio read timed out.";
        return;
    }
    if (gatt.audio_read_handle && !gatt.read_pending && !read_task_active() &&
        (!gatt.read_idle || lv_tick_elaps(gatt.read_started) >= 250u)) {
        read_used = true;
        unload_waiting = false;
        gatt.read_started = lv_tick_get();
        gatt.read_pending = start_att_task(gatt.audio_read_handle, execute_audio_read);
        if (!gatt.read_pending) {
            gatt.audio_read_handle = 0;
            read_failed = true;
        }
    }
    if (read_failed) {
        update->poll_ms = 100;
        update->status = "GATT read failed. Notifications available.";
    } else if (gatt.read_idle && !gatt.read_ready) {
        gatt.read_ready = true;
        update->status = "GATT read ready. Send audio from the Mac.";
    }

    bool audio_changed = audio.state != gatt.audio_state || audio.received != gatt.audio_received;
    if (audio.state != BLE_AUDIO_IDLE || gatt.playing_audio)
        update->length = 0;
    gatt.audio_state = audio.state;
    gatt.audio_received = audio.received;
    if (audio.play_on_receive &&
        (audio.state == BLE_AUDIO_RECEIVING || audio.state == BLE_AUDIO_READY)) {
        update->poll_ms = 25;
        if (hb_pcm_playing())
            return;
        gatt.playing_audio = false;
        uint32_t size = 0;
        bool reserved = ble_audio_reserve_stream();
        gatt_lock(guard);
        if (reserved)
            size = ble_audio_prepare_stream();
        else if (ble_audio_progress().play_on_receive)
            ble_audio_reset();
        gatt_unlock(guard);

        if (size)
            gatt.playing_audio = hb_pcm_play(size);
        if (gatt.playing_audio) {
            update->status = "Playing incoming audio.";
        } else if (size || !reserved) {
            gatt_lock(guard);
            if (ble_audio_progress().play_on_receive)
                ble_audio_reset();
            gatt_unlock(guard);
            update->status = "Could not play incoming audio.";
        } else {
            lv_snprintf(update->text, sizeof(update->text), "Buffering audio... %u%%",
                        (unsigned)(audio.received * 100 / audio.total));
            update->status = update->text;
        }
        return;
    }
    if (!gatt.audio_read_handle && !gatt.playing_audio)
        update->poll_ms = 100;
    if (audio.state == BLE_AUDIO_READY) {
        if (hb_pcm_playing()) {
            update->status = "Clip received. Waiting to play...";
            return;
        }
        gatt.playing_audio = ble_audio_play();
        update->status = gatt.playing_audio
                         ? "Playing received audio." : "Could not play received audio.";
    } else if (audio_changed && audio.state == BLE_AUDIO_RECEIVING) {
        lv_snprintf(update->text, sizeof(update->text), "Receiving audio... %u%%",
                    (unsigned)(audio.received * 100 / audio.total));
        update->status = update->text;
    } else if (audio.state == BLE_AUDIO_ERROR) {
        update->status = "Audio incomplete or invalid. Try again.";
    } else if (audio.state == BLE_AUDIO_CANCELLED) {
        update->status = "Audio transfer cancelled.";
    } else if (gatt.playing_audio && !hb_pcm_playing()) {
        gatt.playing_audio = false;
        hb_pcm_release();
        if (audio.state == BLE_AUDIO_IDLE)
            update->status = "Audio finished.";
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

static const char *connect_uart(const uint8_t address[HB_BLE_ADDRESS_SIZE], bool receive_audio, bool chat)
{
    if (!address || !hb_shared || gatt.ref || read_task_active())
        return "BLE connection is unavailable or already active.";
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
    uintptr_t device = find(manager, address, 0);
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
                status = start_gatt(owner, session, central, ref, changed_mode, receive_audio, chat);
                if (!status) {
                    if (((connect_ble_fn)LE_DISCOVER_SERVICES)(gatt.owner, &gatt.ref))
                        return NULL;
                    finish_gatt();
                    return "Service discovery rejected.";
                }
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


const char *hb_ble_connect(const uint8_t address[HB_BLE_ADDRESS_SIZE])
{
    return connect_uart(address, true, false);
}

const char *hb_ble_connect_notifications(const uint8_t address[HB_BLE_ADDRESS_SIZE])
{
    return connect_uart(address, false, false);
}

const char *hb_ble_connect_chat(const uint8_t address[HB_BLE_ADDRESS_SIZE])
{
    if (*(const volatile uint32_t *)0x080ea60cu != 0x1e0db570u ||
        *(const volatile uint32_t *)0x080ea620u != 0x0012f04fu)
        return "BLE write firmware mismatch.";
    return connect_uart(address, false, true);
}

bool hb_ble_chat_ready(void)
{
    return hb_ble_ready() && gatt.chat_handle && !gatt.chat_pending && !read_task_active();
}

const char *hb_ble_chat_send(const char *text, unsigned length)
{
    if (!hb_ble_chat_ready())
        return "Connect or wait for the current message.";
    if (!text || !length || length > sizeof(chat_message))
        return "Enter up to 20 characters.";
    for (unsigned i = 0; i < length; i++) {
        if ((uint8_t)text[i] < 32 || (uint8_t)text[i] > 126)
            return "Use printable ASCII text.";
    }
    memcpy(chat_message, text, length);
    chat_length = length;
    read_used = true;
    unload_waiting = false;
    gatt.chat_started = lv_tick_get();
    gatt.chat_pending = start_att_task(gatt.chat_handle, execute_chat_write);
    return gatt.chat_pending ? NULL : "Could not queue the BLE write.";
}

bool hb_ble_connected(void) { return gatt.ref != 0; }

bool hb_ble_ready(void) { return gatt.ref && !gatt.expected_cookie; }

void hb_ble_disconnect(void) { finish_gatt(); }

int hb_ble_prepare_unload(void)
{
    hb_ble_disconnect();
    if (read_task_active()) {
        unload_waiting = false;
        return 0;
    }
    /* The broker must finish returning from the read task destructor. */
    if (read_used) {
        if (!unload_waiting) {
            unload_started = lv_tick_get();
            unload_waiting = true;
        }
        if (lv_tick_elaps(unload_started) < 100u)
            return 0;
        read_used = false;
    }
    return hb_pcm_release();
}
#endif
