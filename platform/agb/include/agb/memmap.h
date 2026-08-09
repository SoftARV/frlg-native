#ifndef GUARD_AGB_MEMMAP_H
#define GUARD_AGB_MEMMAP_H

#include <stdint.h>

// Regions are packed rather than laid out at the GBA's sparse guest addresses;
// nothing in the game depends on the distance between them.
struct agb_memory
{
    uint8_t ewram[0x40000];
    uint8_t iwram[0x8000];
    uint8_t io[0x400];
    uint8_t pltt[0x400];
    uint8_t vram[0x18000];
    uint8_t oam[0x400];
} __attribute__((aligned(8)));

extern struct agb_memory agb_mem;

#endif // GUARD_AGB_MEMMAP_H
