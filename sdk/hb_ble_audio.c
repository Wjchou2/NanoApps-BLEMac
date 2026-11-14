#include "hb_ble_audio.h"
#include "hb_pcm.h"
#include <string.h>

#define MAX_FRAMES (30u * HB_PCM_RATE)
#define STREAM_MIN_FRAMES (HB_PCM_RATE / 2u)

/* See tools/macos/nano_uart_sender.swift. Each read or notification is one record.
 * Nonzero first byte remains the existing text protocol. Audio records begin
 * with 0, followed by A (frame count and checksum), Q (same plus a sample
 * rate divisor), S (play as received, with a divisor), D (offset and bytes),
 * or C (cancel). F starts fast notifications with a divisor and a playback
 * flag. Fast data records use a 7-bit sequence instead of a 32-bit offset.
 * Integers are little-endian, and IMA ADPCM samples are packed low nibble first.
 */
static struct {
    ble_audio_progress_t progress;
    uint32_t frames, rate_divisor, expected_hash, hash;
    uint32_t decoded;
    int predictor, index;
    bool fast;
    uint8_t sequence;
    uint8_t compressed[(MAX_FRAMES + 1) / 2];
} transfer;

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

    bool fast_data = (data[1] & 0x80u) != 0;
    switch (fast_data ? 'D' : data[1]) {
    case 'A':
    case 'F':
    case 'S':
    case 'Q': {
        bool fast = data[1] == 'F';
        bool has_divisor = data[1] != 'A';
        if (length != (fast ? 12u : has_divisor ? 11u : 10u) ||
            (fast && data[11] > 1u)) {
            transfer.progress.state = BLE_AUDIO_ERROR;
            break;
        }
        uint32_t frames = word(data + 2);
        uint32_t divisor = has_divisor ? data[10] : 1u;
        if (divisor < (data[1] == 'Q' ? 2u : 1u) || divisor > 3u ||
            !frames || frames > MAX_FRAMES / divisor) {
            transfer.progress.state = BLE_AUDIO_ERROR;
            break;
        }
        transfer.frames = frames;
        transfer.rate_divisor = divisor;
        transfer.expected_hash = word(data + 6);
        transfer.hash = 2166136261u;
        transfer.decoded = 0;
        transfer.predictor = 0;
        transfer.index = 0;
        transfer.fast = fast;
        transfer.sequence = 0;
        transfer.progress = (ble_audio_progress_t){
            BLE_AUDIO_RECEIVING, 0, (frames + 1) / 2,
            data[1] == 'S' || (fast && data[11] == 1u)
        };
        break;
    }
    case 'D': {
        if (transfer.progress.state != BLE_AUDIO_RECEIVING)
            break;
        uint32_t offset = transfer.progress.received;
        unsigned header = fast_data ? 2u : 6u;
        if (fast_data != transfer.fast || length <= header ||
            (fast_data ? (data[1] & 0x7fu) != transfer.sequence : word(data + 2) != offset) ||
            length - header > transfer.progress.total - offset) {
            transfer.progress.state = BLE_AUDIO_ERROR;
            break;
        }
        transfer.sequence = (transfer.sequence + 1u) & 0x7fu;
        data += header;
        length -= header;
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

static void decode_pcm(int16_t *output, uint32_t end)
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
    int predictor = transfer.predictor, index = transfer.index;
    int divisor = (int)transfer.rate_divisor;
    for (uint32_t i = transfer.decoded; i < end; i++) {
        unsigned code = (transfer.compressed[i / 2] >> ((i & 1) * 4)) & 15;
        int step = steps[index];
        int delta = (step >> 3) + ((code & 4) ? step : 0) +
                    ((code & 2) ? step >> 1 : 0) + ((code & 1) ? step >> 2 : 0);
        int previous = predictor;
        predictor += (code & 8) ? -delta : delta;
        if (predictor < -32768) predictor = -32768;
        if (predictor > 32767) predictor = 32767;
        index += index_change[code & 7];
        if (index < 0) index = 0;
        if (index > 88) index = 88;
        if (!i)
            previous = predictor;
        for (int j = 1; j <= divisor; j++) {
            int16_t sample = (int16_t)(previous + (predictor - previous) * j / divisor);
            *output++ = sample;
            *output++ = sample;
        }
    }
    transfer.predictor = predictor;
    transfer.index = index;
    transfer.decoded = end;
}

bool ble_audio_play(void)
{
    if (!hb_pcm_release())
        return false;
    uint32_t size = transfer.frames * transfer.rate_divisor * 4;
    int16_t *samples = hb_pcm_reserve(size, 128u * 1024u);
    if (!samples)
        return false;
    decode_pcm(samples, transfer.frames);
    return hb_pcm_play(size);
}

bool ble_audio_reserve_stream(void)
{
    return hb_pcm_reserve(HB_PCM_RATE * 4u, 0) != NULL;
}

/* Runs under the broker lock. Only decode samples already received. */
uint32_t ble_audio_prepare_stream(void)
{
    if (!transfer.progress.play_on_receive ||
        (transfer.progress.state != BLE_AUDIO_RECEIVING &&
         transfer.progress.state != BLE_AUDIO_READY))
        return 0;
    uint32_t available = transfer.progress.received * 2u;
    if (available > transfer.frames)
        available = transfer.frames;
    uint32_t count = available - transfer.decoded;
    uint32_t divisor = transfer.rate_divisor;
    if (transfer.progress.state == BLE_AUDIO_RECEIVING && count * divisor < STREAM_MIN_FRAMES)
        return 0;
    if (count > HB_PCM_RATE / divisor)
        count = HB_PCM_RATE / divisor;
    decode_pcm(hb_pcm_samples(), transfer.decoded + count);
    if (transfer.decoded == transfer.frames)
        ble_audio_reset();
    return count * divisor * 4u;
}
