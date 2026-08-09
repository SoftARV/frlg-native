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
    uint8_t sram[0x20000];
} __attribute__((aligned(8)));

extern struct agb_memory agb_mem;

// The player's ROM image. Data symbols the host cannot build — the scripts and
// map data in data/*.s — are bound to offsets inside this array at link time by
// tools/gen_cart_syms.py, so game code reaches them as the symbols it always used.
#define AGB_CART_SIZE 0x1000000

extern uint8_t agb_cart[AGB_CART_SIZE];

#endif // GUARD_AGB_MEMMAP_H
