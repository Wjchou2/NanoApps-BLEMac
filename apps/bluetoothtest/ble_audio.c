#include "ble_audio.h"
#include "hb_heap.h"
#include "hb_sdk.h"
#include <string.h>

#define PCM_RATE 22050u
#define MAX_FRAMES (30u * PCM_RATE)
#define SFX_PLAYING 0x64u

/* See tools/macos/nano_uart_sender.swift. Each notification is one record.
 * Nonzero first byte remains the existing text protocol. Audio records begin
 * with 0, followed by A (frame count and checksum), D (offset and bytes), or C.
 * Integers are little-endian, and IMA ADPCM samples are packed low nibble first.
 */
static struct {
    ble_audio_progress_t progress;
    uint32_t frames, expected_hash, hash;
    uint8_t compressed[(MAX_FRAMES + 1) / 2];
} transfer;

static uint32_t descriptor[0x80 / sizeof(uint32_t)];
static int16_t *pcm;

static uint32_t word(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

ble_audio_progress_t ble_audio_progress(void)
{
    return transfer.progress;
}

void ble_audio_reset(void)
{
    transfer.progress = (ble_audio_progress_t){0};
}

bool ble_audio_receive(const uint8_t *data, unsigned length)
{
    if (!length || data[0])
        return false;

    /* Playback decodes this buffer outside the Bluetooth lock. */
    if (transfer.progress.state == BLE_AUDIO_READY || length < 2)
        return true;

    switch (data[1]) {
    case 'A': {
        if (length != 10) {
            transfer.progress.state = BLE_AUDIO_ERROR;
            break;
        }
        uint32_t frames = word(data + 2);
        if (!frames || frames > MAX_FRAMES) {
            transfer.progress.state = BLE_AUDIO_ERROR;
            break;
        }
        transfer.frames = frames;
        transfer.expected_hash = word(data + 6);
        transfer.hash = 2166136261u;
        transfer.progress = (ble_audio_progress_t){BLE_AUDIO_RECEIVING, 0, (frames + 1) / 2};
        break;
    }
    case 'D': {
        if (transfer.progress.state != BLE_AUDIO_RECEIVING)
            break;
        uint32_t offset = transfer.progress.received;
        if (length <= 6 || word(data + 2) != offset ||
            length - 6 > transfer.progress.total - offset) {
            transfer.progress.state = BLE_AUDIO_ERROR;
            break;
        }
        data += 6;
        length -= 6;
        memcpy(transfer.compressed + offset, data, length);
        for (unsigned i = 0; i < length; i++)
            transfer.hash = (transfer.hash ^ data[i]) * 16777619u;
        transfer.progress.received += length;
        if (transfer.progress.received == transfer.progress.total) {
            transfer.progress.state = transfer.hash == transfer.expected_hash
                ? BLE_AUDIO_READY : BLE_AUDIO_ERROR;
        }
        break;
    }
    case 'C':
        if (transfer.progress.state == BLE_AUDIO_RECEIVING)
            transfer.progress.state = BLE_AUDIO_CANCELLED;
        break;
    default:
        break;
    }
    return true;
}

static void decode_pcm(int16_t *output)
{
    static const int steps[] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
        13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };
    static const int index_change[] = {-1, -1, -1, -1, 2, 4, 6, 8};
    int predictor = 0, index = 0;
    for (uint32_t i = 0; i < transfer.frames; i++) {
        unsigned code = (transfer.compressed[i / 2] >> ((i & 1) * 4)) & 15;
        int step = steps[index];
        int delta = (step >> 3) + ((code & 4) ? step : 0) +
                    ((code & 2) ? step >> 1 : 0) + ((code & 1) ? step >> 2 : 0);
        predictor += (code & 8) ? -delta : delta;
        if (predictor < -32768) predictor = -32768;
        if (predictor > 32767) predictor = 32767;
        index += index_change[code & 7];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        output[2 * i] = output[2 * i + 1] = (int16_t)predictor;
    }
}

bool ble_audio_playing(void)
{
    return pcm && ((const volatile uint8_t *)descriptor)[SFX_PLAYING];
}

bool ble_audio_release(void)
{
    if (ble_audio_playing())
        return false;
    if (pcm) {
        hb_os_free(pcm);
        pcm = NULL;
    }
    return true;
}

bool ble_audio_play(void)
{
    if (!ble_audio_release())
        return false;
    uint32_t size = transfer.frames * 4;
    if (hb_os_heap_largest() < size + 128u * 1024u)
        return false;
    pcm = hb_os_alloc(size);
    if (!pcm)
        return false;
    decode_pcm(pcm);

    /* Same 1.1.2 linear PCM descriptor layout as Entrain. The native
     * voice sets its ownership flag synchronously in setSource. */
    hb_audio_ctor(descriptor);
    descriptor[0x04 / 4] = descriptor[0x08 / 4] = (uint32_t)(uintptr_t)pcm;
    descriptor[0x0c / 4] = size;
    ((uint8_t *)descriptor)[0x10] = 0;  /* Linear PCM. */
    descriptor[0x14 / 4] = PCM_RATE;
    descriptor[0x18 / 4] = 2;
    descriptor[0x1c / 4] = 16;
    hb_audio_setfields(descriptor, 0x4000);
    hb_audio_play_now(descriptor);
    if (ble_audio_playing())
        return true;
    ble_audio_release();
    return false;
}
