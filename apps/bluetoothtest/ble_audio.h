#ifndef BLE_AUDIO_H
#define BLE_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BLE_AUDIO_IDLE, BLE_AUDIO_RECEIVING, BLE_AUDIO_READY, BLE_AUDIO_ERROR,
    BLE_AUDIO_CANCELLED
} ble_audio_state_t;

typedef struct {
    ble_audio_state_t state;
    uint32_t received;
    uint32_t total;
} ble_audio_progress_t;

/* Call under the Bluetooth broker lock, except for playback and cleanup. */
bool ble_audio_receive(const uint8_t *data, unsigned length);
ble_audio_progress_t ble_audio_progress(void);
void ble_audio_reset(void);

/* A ready clip stays immutable until playback has been started on the UI task. */
bool ble_audio_play(void);
bool ble_audio_playing(void);
bool ble_audio_release(void);

#endif
