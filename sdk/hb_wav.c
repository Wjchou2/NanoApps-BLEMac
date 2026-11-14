#include "hb_wav.h"
#include "hb_pcm.h"
#include "hb_sdk.h"
#include <string.h>

static struct {
    bool open, failed;
    uint32_t rate, output_rate, stride, phase;
    uint32_t output_left, source_frames, source_index;
    uint32_t cache_first, cache_count;
    uint8_t cache[4096];
} wav;

static uint32_t word(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool skip_bytes(uint32_t count)
{
    while (count) {
        uint32_t chunk = count < sizeof(wav.cache) ? count : sizeof(wav.cache);
        if (hb_fs_reader_read(wav.cache, chunk) != chunk)
            return false;
        count -= chunk;
    }
    return true;
}

static bool parse(const char *path, uint32_t size, bool high_rate)
{
    uint8_t header[12], format[16];
    if (size < sizeof(header) || hb_fs_reader_read(header, sizeof(header)) != sizeof(header) ||
        memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVE", 4) ||
        word(header + 4) != size - 8)
        return false;

    bool have_format = false;
    uint32_t offset = sizeof(header), data_offset = 0, data_size = 0;
    while (size - offset >= 8) {
        uint8_t chunk[8];
        if (hb_fs_reader_read(chunk, sizeof(chunk)) != sizeof(chunk))
            return false;
        offset += sizeof(chunk);
        uint32_t length = word(chunk + 4);
        if (length > size - offset || (length & 1u) > size - offset - length)
            return false;
        uint32_t padded = length + (length & 1u);
        if (!have_format && length >= sizeof(format) && !memcmp(chunk, "fmt ", 4)) {
            if (hb_fs_reader_read(format, sizeof(format)) != sizeof(format))
                return false;
            have_format = true;
            offset += sizeof(format);
            padded -= sizeof(format);
        } else if (!data_offset && !memcmp(chunk, "data", 4)) {
            data_offset = offset;
            data_size = length;
        }
        if (have_format && data_offset)
            break;
        if (!skip_bytes(padded))
            return false;
        offset += padded;
    }

    if (!have_format || !data_offset || format[0] != 1 || format[1] != 0 ||
        format[3] != 0 || format[14] != 16 || format[15] != 0)
        return false;
    unsigned channels = format[2];
    uint32_t rate = word(format + 4);
    if ((channels != 1 && channels != 2) || rate < 4000 || rate > 96000)
        return false;
    unsigned stride = channels * 2u;
    if (!data_size || data_size % stride || format[12] != stride || format[13] != 0)
        return false;
    if (offset != data_offset) {
        hb_fs_reader_close();
        if (!hb_fs_reader_open(path) || !skip_bytes(data_offset))
            return false;
    }

    wav.output_rate = high_rate && rate > HB_PCM_RATE ? 44100u : HB_PCM_RATE;
    uint64_t frames = (uint64_t)(data_size / stride) * wav.output_rate / rate;
    if (!frames || frames > UINT32_MAX)
        return false;
    wav.rate = rate;
    wav.stride = stride;
    wav.source_frames = data_size / stride;
    wav.output_left = (uint32_t)frames;
    return true;
}

const char *hb_wav_open_native(const char *path, uint32_t max_size,
                               uint32_t *frames, uint32_t *rate)
{
    if (wav.open)
        return "Audio is already open.";
    uint32_t size = hb_fs_size(path);
    if (!size || size > max_size)
        return "File missing or too large to play.";
    if (!hb_fs_reader_open(path))
        return "Could not open the WAV file.";
    wav.open = true;
    wav.failed = false;
    wav.phase = wav.source_index = wav.cache_first = wav.cache_count = 0;
    if (!parse(path, size, rate != NULL)) {
        hb_wav_close();
        return "Could not read this WAV. Resend it from the Mac.";
    }
    *frames = wav.output_left;
    if (rate)
        *rate = wav.output_rate;
    return NULL;
}

const char *hb_wav_open(const char *path, uint32_t max_size, uint32_t *frames)
{
    return hb_wav_open_native(path, max_size, frames, NULL);
}

uint32_t hb_wav_read(int16_t *output, uint32_t frames)
{
    if (!wav.open || wav.failed)
        return 0;
    if (frames > wav.output_left)
        frames = wav.output_left;
    uint32_t i = 0;
    for (; i < frames; i++) {
        while (wav.source_index >= wav.cache_first + wav.cache_count) {
            wav.cache_first += wav.cache_count;
            uint32_t available = wav.source_frames - wav.cache_first;
            uint32_t capacity = sizeof(wav.cache) / wav.stride;
            wav.cache_count = available < capacity ? available : capacity;
            uint32_t bytes = wav.cache_count * wav.stride;
            if (!bytes || hb_fs_reader_read(wav.cache, bytes) != bytes) {
                wav.failed = true;
                return i;
            }
        }
        const uint8_t *left = wav.cache + (wav.source_index - wav.cache_first) * wav.stride;
        const uint8_t *right = left + wav.stride - 2;
        *output++ = (int16_t)(left[0] | (uint16_t)left[1] << 8);
        *output++ = (int16_t)(right[0] | (uint16_t)right[1] << 8);
        wav.phase += wav.rate;
        wav.source_index += wav.phase / wav.output_rate;
        wav.phase %= wav.output_rate;
    }
    wav.output_left -= i;
    return i;
}

bool hb_wav_failed(void)
{
    return wav.failed;
}

void hb_wav_close(void)
{
    if (wav.open) {
        hb_fs_reader_close();
        wav.open = false;
    }
}
