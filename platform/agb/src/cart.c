// Loading a ROM image into the cart region, and relocating the pointers in it.
//
// The image holds the addresses the original build gave its own code and RAM.
// None of them mean anything in this address space, so each is rewritten once
// here. Which ones, and to what, is worked out at build time -- see
// tools/gen_relocations.py and docs/spikes/0005-relocation-classes.md.

#include <stdio.h>
#include <string.h>

#include "agb/cart.h"
#include "agb/memmap.h"

// Where the original's ROM began. A pointer into ROM data becomes the same
// distance into the region we loaded it at.
#define GBA_ROM_BASE 0x08000000u

static uint32_t read32(uint32_t offset)
{
    uint32_t value;

    memcpy(&value, agb_cart + offset, sizeof(value));
    return value;
}

static void write32(uint32_t offset, uint32_t value)
{
    memcpy(agb_cart + offset, &value, sizeof(value));
}

void agb_cart_relocate(void)
{
    for (uint32_t i = 0; i < agb_reloc_data_count; i++)
    {
        uint32_t offset = agb_reloc_data[i];

        write32(offset, (uint32_t)(uintptr_t)(agb_cart + (read32(offset) - GBA_ROM_BASE)));
    }

    for (uint32_t i = 0; i < agb_reloc_symbol_count; i++)
    {
        const struct agb_reloc_symbol *r = &agb_reloc_symbols[i];

        write32(r->offset, (uint32_t)((uintptr_t)r->target + r->addend));
    }
}

int agb_cart_load(const char *path)
{
    FILE *fh = fopen(path, "rb");
    size_t read;

    if (fh == NULL)
        return -1;

    read = fread(agb_cart, 1, AGB_CART_SIZE, fh);
    fclose(fh);

    // The relocation table addresses this image by offset, so an image of the
    // wrong length is not one the table describes.
    if (read != AGB_CART_SIZE)
    {
        memset(agb_cart, 0, AGB_CART_SIZE);
        return -2;
    }

    agb_cart_relocate();
    return 0;
}
