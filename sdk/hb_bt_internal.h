#ifndef HB_BT_INTERNAL_H
#define HB_BT_INTERNAL_H
#include <stdbool.h>
#include <stdint.h>

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
#define BT_CLIENT_PAIR_AGENT 0x8cu
#define BT_CLIENT_START_SLOT 0x44u
#define BT_CLIENT_START 0x08455c2du
#define BT_CLIENT_LOCK 0x08424a8bu
#define BT_CLIENT_UNLOCK 0x08424a7du
#define BT_CLASSIC_MODULE 1u
#define BT_MODEL_SLOT 0x08812d7cu
#define BT_MODEL_VTABLE 0x087b57e0u

static inline bool firmware_matches(void)
{
    /* Accept the stock instructions or NanoApps' existing timestamp fix. */
    const volatile uint16_t *code = (const volatile uint16_t *)A2DP_TIMESTAMP_CODE;
    return (code[0] == 0xee01 && code[1] == 0xca10 &&
            code[2] == 0xed91 && code[3] == 0x0a01) ||
           (code[0] == 0x6848 && code[1] == 0xeb00 &&
            code[2] == 0x10cc && code[3] == 0x6048);
}

static inline bool in_sdram(uintptr_t address, uint32_t size)
{
    return address >= SDRAM_START && address < SDRAM_END &&
           size <= SDRAM_END - address;
}

static inline uintptr_t read_pointer(uintptr_t address)
{
    if (address % sizeof(uintptr_t) || !in_sdram(address, sizeof(uintptr_t)))
        return 0;
    return *(const volatile uintptr_t *)address;
}

#endif
