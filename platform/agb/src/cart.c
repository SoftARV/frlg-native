// Loading a ROM image into the cart region, and relocating the pointers in it.
//
// The image holds the addresses the original build gave its own code and RAM.
// None of them mean anything in this address space, so each is rewritten once
// here. Which ones, and to what, is worked out at build time -- see
// tools/gen_relocations.py and docs/spikes/0005-relocation-classes.md.

#include <stdio.h>
#include <string.h>

#include "agb/cart.h"
#include "agb/sha1.h"
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

// The image is only meaningful to the build that relocated it: `target` in every
// symbol record is an address this binary chose, and the next link can choose
// differently. Hashing those addresses is a cheap exact answer to "was this
// cache written by me", where a version number would be a promise somebody has
// to remember to keep.
uint32_t agb_cart_layout_id(void)
{
    uint32_t hash = 2166136261u;   // FNV-1a
    const uint8_t *base = (const uint8_t *)&agb_cart[0];

    hash = (hash ^ agb_reloc_data_count) * 16777619u;
    hash = (hash ^ agb_reloc_symbol_count) * 16777619u;
    hash = (hash ^ (uint32_t)(uintptr_t)base) * 16777619u;
    for (uint32_t i = 0; i < agb_reloc_symbol_count; i++)
    {
        hash = (hash ^ (uint32_t)(uintptr_t)agb_reloc_symbols[i].target) * 16777619u;
        hash = (hash ^ agb_reloc_symbols[i].offset) * 16777619u;
    }
    return hash;
}

int agb_cart_import(const char *path, const char *wanted, char saw[AGB_SHA1_TEXT])
{
    uint8_t digest[AGB_SHA1_SIZE];
    FILE *fh = fopen(path, "rb");
    size_t read;

    saw[0] = '\0';
    if (fh == NULL)
        return AGB_CART_UNREADABLE;

    read = fread(agb_cart, 1, AGB_CART_SIZE, fh);
    fclose(fh);
    if (read != AGB_CART_SIZE)
    {
        memset(agb_cart, 0, AGB_CART_SIZE);
        return AGB_CART_WRONG_SIZE;
    }

    // Hashed before relocation, because relocation rewrites the image: the
    // answer has to be about the file the player supplied, not about what this
    // port made of it.
    agb_sha1(agb_cart, AGB_CART_SIZE, digest);
    agb_sha1_format(digest, saw);

    if (wanted != NULL && wanted[0] != '\0')
    {
        uint8_t expect[AGB_SHA1_SIZE];

        if (agb_sha1_parse(wanted, expect) != 0
            || memcmp(digest, expect, sizeof(digest)) != 0)
        {
            memset(agb_cart, 0, AGB_CART_SIZE);
            return AGB_CART_WRONG_GAME;
        }
    }

    agb_cart_relocate();
    return AGB_CART_OK;
}

// Enough of a header to refuse a cache that does not belong to this build or
// this ROM. Not a format anyone else reads, so it is only as complicated as
// those two questions.
struct cart_cache_header
{
    char magic[8];
    uint32_t version;
    uint32_t layout;
    uint32_t size;
    char rom_sha1[AGB_SHA1_TEXT];
};

#define CART_CACHE_MAGIC "FRLGCART"
#define CART_CACHE_VERSION 1

int agb_cart_cache_save(const char *path, const char *rom_sha1)
{
    struct cart_cache_header header;
    FILE *fh = fopen(path, "wb");

    if (fh == NULL)
        return -1;

    memset(&header, 0, sizeof(header));
    memcpy(header.magic, CART_CACHE_MAGIC, sizeof(header.magic));
    header.version = CART_CACHE_VERSION;
    header.layout = agb_cart_layout_id();
    header.size = AGB_CART_SIZE;
    snprintf(header.rom_sha1, sizeof(header.rom_sha1), "%s", rom_sha1 ? rom_sha1 : "");

    if (fwrite(&header, sizeof(header), 1, fh) != 1
        || fwrite(agb_cart, 1, AGB_CART_SIZE, fh) != AGB_CART_SIZE)
    {
        fclose(fh);
        remove(path);   // A half-written cache is worse than none.
        return -1;
    }
    fclose(fh);
    return 0;
}

int agb_cart_cache_load(const char *path, const char *rom_sha1)
{
    struct cart_cache_header header;
    FILE *fh = fopen(path, "rb");
    size_t read;

    if (fh == NULL)
        return -1;

    if (fread(&header, sizeof(header), 1, fh) != 1
        || memcmp(header.magic, CART_CACHE_MAGIC, sizeof(header.magic)) != 0
        || header.version != CART_CACHE_VERSION
        || header.size != AGB_CART_SIZE
        || header.layout != agb_cart_layout_id()
        || (rom_sha1 != NULL && strcmp(header.rom_sha1, rom_sha1) != 0))
    {
        fclose(fh);
        return -1;
    }

    read = fread(agb_cart, 1, AGB_CART_SIZE, fh);
    fclose(fh);
    if (read != AGB_CART_SIZE)
    {
        memset(agb_cart, 0, AGB_CART_SIZE);
        return -1;
    }
    return 0;   // Already relocated when it was written.
}
