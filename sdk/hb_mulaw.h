#ifndef HB_MULAW_H
#define HB_MULAW_H

#include <stdint.h>

static inline int16_t hb_mulaw_decode(uint8_t byte)
{
    unsigned code = byte ^ 0xffu;
    int value = (int)(((code & 15u) * 8u + 132u) << ((code >> 4u) & 7u)) - 132;
    return (int16_t)((code & 128u) ? -value : value);
}

#endif
