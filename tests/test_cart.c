// The relocation pass over a loaded cart image.
//
// The pass itself is small; what it has to get right is which arithmetic each
// class of record gets, and -- just as much -- leaving alone everything the
// table does not name. The generated table is not linked here: a test supplies
// its own, the way the PPU tests supply an interrupt table.

#include <string.h>

#include "agb/cart.h"
#include "agb/memmap.h"

#include "harness.h"

#define GBA_ROM_BASE 0x08000000u

// Stand-ins for whatever the generated table would name.
static char code_target[4];
static char ram_target[64];

// Offsets in the image that the table describes. Deliberately not adjacent, so
// a pass that walked the image rather than the table would be caught.
#define DATA_SITE 0x100
#define CODE_SITE 0x200
#define RAM_SITE 0x300
#define UNTOUCHED_SITE 0x400

const uint32_t agb_reloc_data[] = {DATA_SITE};
const uint32_t agb_reloc_data_count = 1;

const struct agb_reloc_symbol agb_reloc_symbols[] = {
    {CODE_SITE, code_target, 0},
    {RAM_SITE, ram_target, 12},
};
const uint32_t agb_reloc_symbol_count = 2;

static uint32_t at(uint32_t offset)
{
    uint32_t v;

    memcpy(&v, agb_cart + offset, sizeof(v));
    return v;
}

static void put(uint32_t offset, uint32_t value)
{
    memcpy(agb_cart + offset, &value, sizeof(value));
}

// A pointer into ROM data becomes the same distance into the region we loaded
// the image at.
static void test_data_pointer(void)
{
    TEST_CASE("a data pointer is shifted to the cart");
    memset(agb_cart, 0, 0x500);

    put(DATA_SITE, GBA_ROM_BASE + 0x1234);
    agb_cart_relocate();

    CHECK(at(DATA_SITE) == (uint32_t)(uintptr_t)(agb_cart + 0x1234),
          "the data pointer came out %08X, not the cart plus 0x1234", at(DATA_SITE));
}

// A pointer to a function becomes ours of that name. The Thumb bit the original
// carried is not part of the address, and the linker has already dropped it by
// resolving the symbol.
static void test_code_pointer(void)
{
    TEST_CASE("a code pointer becomes the native symbol");
    memset(agb_cart, 0, 0x500);

    put(CODE_SITE, GBA_ROM_BASE + 0x5679); // an address with the Thumb bit set
    agb_cart_relocate();

    CHECK(at(CODE_SITE) == (uint32_t)(uintptr_t)code_target,
          "the code pointer came out %08X, not the native symbol", at(CODE_SITE));
    CHECK((at(CODE_SITE) & 1) == 0, "the relocated pointer kept a Thumb bit");
}

// A RAM pointer carries however far past its symbol it pointed.
static void test_ram_pointer_with_addend(void)
{
    TEST_CASE("a RAM pointer keeps its addend");
    memset(agb_cart, 0, 0x500);

    put(RAM_SITE, 0x02000000);
    agb_cart_relocate();

    CHECK(at(RAM_SITE) == (uint32_t)((uintptr_t)ram_target + 12),
          "the RAM pointer came out %08X, not the symbol plus twelve", at(RAM_SITE));
}

// Everything the table does not name is left exactly as it was. Most of the
// image is data our own build re-creates and never reads, and rewriting a word
// there on a guess would corrupt it.
static void test_leaves_the_rest_alone(void)
{
    TEST_CASE("words the table does not name are untouched");
    memset(agb_cart, 0, 0x500);

    // A word that looks exactly like a pointer, but is not in the table.
    put(UNTOUCHED_SITE, GBA_ROM_BASE + 0x1234);
    agb_cart_relocate();

    CHECK(at(UNTOUCHED_SITE) == GBA_ROM_BASE + 0x1234,
          "a word outside the table was rewritten to %08X", at(UNTOUCHED_SITE));
}

// An image of the wrong length is not one the table describes, so it is refused
// rather than half-applied.
static void test_rejects_a_short_image(void)
{
    const char *path = "/etc/hostname"; // any file that is not 16 MiB
    int err;

    TEST_CASE("an image of the wrong size is refused");
    memset(agb_cart, 0xAB, 0x100);

    err = agb_cart_load(path);

    CHECK(err == -2, "a short image returned %d, not -2", err);
    CHECK(agb_cart[0] == 0 && agb_cart[0x80] == 0,
          "a refused image left its bytes behind");

    TEST_CASE("a missing file is reported rather than loaded");
    CHECK(agb_cart_load("/nonexistent/rom.gba") == -1,
          "a missing file did not report as unreadable");
}

int main(void)
{
    test_data_pointer();
    test_code_pointer();
    test_ram_pointer_with_addend();
    test_leaves_the_rest_alone();
    test_rejects_a_short_image();

    return test_report("cart relocation");
}
