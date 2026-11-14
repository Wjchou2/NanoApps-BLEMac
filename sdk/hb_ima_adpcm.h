#ifndef HB_IMA_ADPCM_H
#define HB_IMA_ADPCM_H

#include <stdint.h>

static inline int16_t hb_ima_predictor(const uint8_t *header)
{
    return (int16_t)(header[0] | (unsigned)header[1] << 8);
}

static inline int16_t hb_ima_sample(unsigned nibble, int *predictor, int *index)
{
    static const uint16_t steps[] = {
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31,
        34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
        157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544,
        598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707,
        1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
        5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635,
        13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767
    };
    static const int8_t adjustments[] = {-1, -1, -1, -1, 2, 4, 6, 8};
    int step = steps[*index];
    int delta = (step >> 3) + (nibble & 1u ? step >> 2 : 0) +
        (nibble & 2u ? step >> 1 : 0) + (nibble & 4u ? step : 0);
    int value = *predictor + (nibble & 8u ? -delta : delta);
    *predictor = value < -32768 ? -32768 : value > 32767 ? 32767 : value;
    int next = *index + adjustments[nibble & 7u];
    *index = next < 0 ? 0 : next > 88 ? 88 : next;
    return (int16_t)*predictor;
}

#endif
