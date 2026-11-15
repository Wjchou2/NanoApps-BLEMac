#include "hb_ble.h"
#include "hb_sdk.h"
#include "hb_shared.h"
#include "lvgl/lvgl.h"
#include <string.h>

static lv_obj_t *status, *received, *input, *send_button, *connect_label;
static lv_timer_t *poll_timer;

static bool mac_beacon(const hb_ble_device_t *device)
{
    return !strcmp(device->name, "Nano Bluetooth Link");
}

static void refresh_controls(void)
{
    const char *action = hb_ble_connected() ? "Disconnect" : "Connect";
    if (strcmp(lv_label_get_text(connect_label), action))
        lv_label_set_text(connect_label, action);
    if (hb_ble_chat_ready())
        lv_obj_remove_state(send_button, LV_STATE_DISABLED);
    else
        lv_obj_add_state(send_button, LV_STATE_DISABLED);
}

static void poll_link(lv_timer_t *timer)
{
    hb_ble_update_t update;
    hb_ble_poll(&update);
    if (update.status)
        lv_label_set_text(status, update.status);
    if (update.length) {
        char text[HB_BLE_NOTIFICATION_LIMIT + 1];
        for (unsigned i = 0; i < update.length; i++)
            text[i] = update.data[i] >= 32 && update.data[i] <= 126 ? (char)update.data[i] : '?';
        text[update.length] = '\0';
        lv_label_set_text_fmt(received, "Mac: %s", text);
    }
    lv_timer_set_period(timer, update.poll_ms);
    refresh_controls();
}

static void toggle_link(lv_event_t *event)
{
    (void)event;
    if (hb_ble_connected()) {
        hb_ble_disconnect();
        lv_label_set_text(status, "Disconnected.");
        refresh_controls();
        return;
    }

    hb_ble_device_t devices[HB_BLE_SCAN_LIMIT];
    unsigned count;
    lv_label_set_text(status, "Looking for the Mac…");
    lv_refr_now(lv_display_get_default());
    const char *error = hb_ble_scan(devices, &count, mac_beacon);
    if (!error && count == 1) {
        lv_label_set_text(status, "Connecting…");
        lv_refr_now(lv_display_get_default());
        error = hb_ble_connect_chat(devices[0].address);
    } else if (!error) {
        error = count ? "More than one Mac is advertising." : "Open the BLE tab on the Mac and try again.";
    }
    lv_label_set_text(status, error ? error : "Discovering BLE services…");
    refresh_controls();
}

static void send_text(lv_event_t *event)
{
    (void)event;
    const char *text = lv_textarea_get_text(input);
    const char *error = hb_ble_chat_send(text, (unsigned)strlen(text));
    lv_label_set_text(status, error ? error : "Sending…");
    refresh_controls();
}

static lv_obj_t *button(lv_obj_t *screen, const char *title, int width, int x, int y,
                        lv_event_cb_t callback, lv_obj_t **label)
{
    lv_obj_t *object = lv_button_create(screen);
    lv_obj_set_size(object, width, 40);
    lv_obj_align(object, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_add_event_cb(object, callback, LV_EVENT_CLICKED, NULL);
    *label = lv_label_create(object);
    lv_label_set_text(*label, title);
    lv_obj_center(*label);
    return object;
}

static int prepare_unload(void)
{
    if (!hb_ble_prepare_unload())
        return 0;
    if (poll_timer) {
        lv_timer_delete(poll_timer);
        poll_timer = NULL;
    }
    return 1;
}

HB_APP_ENTRY(payload_entry)
{
    hb_shared->prepare_unload = prepare_unload;
    lv_obj_t *screen = lv_screen_active();
    lv_obj_t *title = lv_label_create(screen);
    lv_label_set_text(title, "BLE Chat");
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 37);

    button(screen, "Connect", 208, 16, 67, toggle_link, &connect_label);
    status = lv_label_create(screen);
    lv_obj_set_width(status, 208);
    lv_obj_set_style_text_align(status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(status, LV_ALIGN_TOP_MID, 0, 116);
    lv_label_set_text(status, "Open the BLE tab on the Mac.");

    received = lv_label_create(screen);
    lv_obj_set_width(received, 208);
    lv_obj_align(received, LV_ALIGN_TOP_MID, 0, 165);
    lv_label_set_text(received, "");

    input = lv_textarea_create(screen);
    lv_textarea_set_one_line(input, true);
    lv_textarea_set_max_length(input, HB_BLE_CHAT_LIMIT);
    lv_textarea_set_placeholder_text(input, "Message");
    lv_obj_set_size(input, 150, 40);
    lv_obj_align(input, LV_ALIGN_TOP_LEFT, 12, 246);
    lv_obj_t *label;
    send_button = button(screen, "Send", 62, 166, 246, send_text, &label);

    lv_obj_t *keyboard = lv_keyboard_create(screen);
    lv_obj_set_size(keyboard, 240, 126);
    lv_obj_align(keyboard, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_keyboard_set_textarea(keyboard, input);
    lv_obj_add_event_cb(keyboard, send_text, LV_EVENT_READY, NULL);
    poll_timer = lv_timer_create(poll_link, 50, NULL);
    refresh_controls();
}
