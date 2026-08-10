// Mosaic: the pixel at the start of each block is repeated across it.
//
// Backgrounds and objects have separate size fields and separate enables, and
// each layer opts in for itself -- so most of what there is to get wrong is in
// the wiring rather than the arithmetic.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define OBJ_VRAM 0x10000
#define OBJ_PLTT 0x200
#define SCREEN_BLOCK 0x800
#define CHAR_BLOCK 0x4000

#define DISPCNT 0x000
#define BG0CNT 0x008
#define BG2CNT 0x00C
#define BG2PA 0x020
#define MOSAIC 0x04C

#define DISPCNT_BG0 0x0100
#define DISPCNT_BG2 0x0400
#define DISPCNT_OBJ 0x1000
#define DISPCNT_1D 0x0040

#define BGCNT_MOSAIC 0x0040
#define BGCNT_SCREEN_BASE(n) ((n) << 8)

#define ATTR0_MOSAIC 0x1000

// Both halves of the register hold one less than the block size.
#define MOSAIC_BG(h, v) (uint16_t)(((h) - 1) | (((v) - 1) << 4))
#define MOSAIC_OBJ(h, v) (uint16_t)((((h) - 1) << 8) | (((v) - 1) << 12))

#define ONE 0x100

#define RED 0x001F
#define GREEN 0x03E0
#define BLUE 0x7C00

static uint32_t argb(uint16_t bgr)
{
    uint32_t r = (bgr & 0x1F) << 3;
    uint32_t g = ((bgr >> 5) & 0x1F) << 3;
    uint32_t b = ((bgr >> 10) & 0x1F) << 3;

    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return (r << 16) | (g << 8) | b;
}

static void io16w(int offset, uint16_t value)
{
    *(volatile uint16_t *)(agb_mem.io + offset) = value;
}

static void io32w(int offset, uint32_t value)
{
    *(volatile uint32_t *)(agb_mem.io + offset) = value;
}

static void bg_pal(int index, uint16_t colour)
{
    *(volatile uint16_t *)(agb_mem.pltt + index * 2) = colour;
}

static uint32_t px(int x, int y)
{
    return agb_ppu_framebuffer()[y * agb_ppu_width() + x];
}

// A 4bpp text BG0 whose first tile is a vertical stripe pattern: column c of
// the tile carries colour index c, so a horizontal mosaic is directly readable
// off the screen. Row r likewise carries index r down the left edge.
static void reset_text_bg(const char *name, uint16_t control)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));

    io16w(DISPCNT, DISPCNT_BG0);
    io16w(BG0CNT, BGCNT_SCREEN_BASE(1) | control);

    // Tile 0, 4bpp: pixel (x, y) gets colour index x + 1 on row 0, and y + 1
    // down column 0, which is enough to read either axis independently.
    for (int y = 0; y < 8; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            int index = y == 0 ? x + 1 : (x == 0 ? y + 1 : 0);
            uint8_t *p = agb_mem.vram + y * 4 + (x >> 1);

            *p = (x & 1) ? (uint8_t)((*p & 0x0F) | (index << 4))
                         : (uint8_t)((*p & 0xF0) | index);
        }
    }
    for (int i = 1; i <= 8; i++)
        bg_pal(i, (uint16_t)(i << 5)); // a distinct green per index
}

static uint16_t index_colour(int index)
{
    return (uint16_t)(index << 5);
}

static void test_background_horizontal(void)
{
    reset_text_bg("bg horizontal mosaic", BGCNT_MOSAIC);
    io16w(MOSAIC, MOSAIC_BG(4, 1));
    agb_ppu_render_frame();

    // Screen columns 0-3 all sample column 0, columns 4-7 all sample column 4.
    for (int x = 0; x < 4; x++)
        CHECK(px(x, 0) == argb(index_colour(1)), "column %d did not repeat column 0", x);
    for (int x = 4; x < 8; x++)
        CHECK(px(x, 0) == argb(index_colour(5)), "column %d did not repeat column 4", x);
}

static void test_background_vertical(void)
{
    reset_text_bg("bg vertical mosaic", BGCNT_MOSAIC);
    io16w(MOSAIC, MOSAIC_BG(1, 4));
    agb_ppu_render_frame();

    // Rows 1-3 repeat row 0, whose left pixel is index 1.
    for (int y = 0; y < 4; y++)
        CHECK(px(0, y) == argb(index_colour(1)), "row %d did not repeat row 0", y);
    // Row 4 starts a new block and shows its own left pixel, index 5.
    CHECK(px(0, 4) == argb(index_colour(5)), "row 4 did not start a new block");
}

// The register alone does nothing; the layer has to ask for it.
static void test_background_enable_bit(void)
{
    reset_text_bg("bg mosaic needs its control bit", 0);
    io16w(MOSAIC, MOSAIC_BG(4, 4));
    agb_ppu_render_frame();

    CHECK(px(1, 0) == argb(index_colour(2)), "mosaic applied without the BGCNT bit");
    CHECK(px(0, 1) == argb(index_colour(2)), "mosaic applied without the BGCNT bit");
}

// A size field of zero means a block of one pixel, which changes nothing.
static void test_size_one_is_identity(void)
{
    reset_text_bg("mosaic size of one", BGCNT_MOSAIC);
    io16w(MOSAIC, MOSAIC_BG(1, 1));
    agb_ppu_render_frame();

    CHECK(px(1, 0) == argb(index_colour(2)), "a size of one altered the picture");
    CHECK(px(3, 0) == argb(index_colour(4)), "a size of one altered the picture");
}

// Objects read the upper half of the register, backgrounds the lower.
static void test_background_and_object_sizes_are_separate(void)
{
    reset_text_bg("bg ignores the object sizes", BGCNT_MOSAIC);
    io16w(MOSAIC, MOSAIC_OBJ(8, 8)); // object fields set, background fields zero
    agb_ppu_render_frame();

    CHECK(px(1, 0) == argb(index_colour(2)), "the background used the object mosaic sizes");
}

// An 8x8 object whose row 0 carries a different colour index per column.
static void add_striped_object(uint16_t extra_attr0)
{
    volatile uint16_t *entry = (volatile uint16_t *)agb_mem.oam;

    for (int y = 0; y < 8; y++)
    {
        for (int x = 0; x < 8; x++)
        {
            int index = y == 0 ? x + 1 : (x == 0 ? y + 1 : 0);
            uint8_t *p = agb_mem.vram + OBJ_VRAM + 32 + y * 4 + (x >> 1);

            *p = (x & 1) ? (uint8_t)((*p & 0x0F) | (index << 4))
                         : (uint8_t)((*p & 0xF0) | index);
        }
    }
    for (int i = 1; i <= 8; i++)
        *(volatile uint16_t *)(agb_mem.pltt + OBJ_PLTT + i * 2) = (uint16_t)(i << 5);

    entry[0] = (uint16_t)(50 | extra_attr0);
    entry[1] = 100;
    entry[2] = 1;
}

static void test_object_horizontal(void)
{
    TEST_CASE("object horizontal mosaic");
    memset(&agb_mem, 0, sizeof(agb_mem));
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    add_striped_object(ATTR0_MOSAIC);
    io16w(MOSAIC, MOSAIC_OBJ(4, 1));
    agb_ppu_render_frame();

    for (int i = 0; i < 4; i++)
        CHECK(px(100 + i, 50) == argb(index_colour(1)),
              "object column %d did not repeat column 0", i);
    for (int i = 4; i < 8; i++)
        CHECK(px(100 + i, 50) == argb(index_colour(5)),
              "object column %d did not repeat column 4", i);
}

static void test_object_enable_bit(void)
{
    TEST_CASE("object mosaic needs its attribute bit");
    memset(&agb_mem, 0, sizeof(agb_mem));
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    add_striped_object(0);
    io16w(MOSAIC, MOSAIC_OBJ(4, 4));
    agb_ppu_render_frame();

    CHECK(px(101, 50) == argb(index_colour(2)), "mosaic applied without the OAM bit");
}

static void test_object_ignores_background_sizes(void)
{
    TEST_CASE("object ignores the background sizes");
    memset(&agb_mem, 0, sizeof(agb_mem));
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    add_striped_object(ATTR0_MOSAIC);
    io16w(MOSAIC, MOSAIC_BG(8, 8)); // background fields set, object fields zero
    agb_ppu_render_frame();

    CHECK(px(101, 50) == argb(index_colour(2)), "the object used the background mosaic sizes");
}

// The affine background carries its own copy of the snapping, so masking a text
// background proves nothing about it.
static void test_affine_background(void)
{
    TEST_CASE("affine bg mosaic");
    memset(&agb_mem, 0, sizeof(agb_mem));

    io16w(DISPCNT, 1 | DISPCNT_BG2); // mode 1 puts BG2 on the affine path
    io16w(BG2CNT, BGCNT_SCREEN_BASE(1) | BGCNT_MOSAIC);
    io16w(BG2PA, ONE);
    io16w(BG2PA + 6, ONE);
    io32w(BG2PA + 8, 0);
    io32w(BG2PA + 12, 0);
    // Affine maps are 8bpp: tile 0 row 0 carries a different index per column.
    for (int x = 0; x < 8; x++)
        agb_mem.vram[x] = (uint8_t)(x + 1);
    for (int y = 1; y < 8; y++)
        agb_mem.vram[y * 8] = (uint8_t)(y + 1);
    for (int i = 1; i <= 8; i++)
        bg_pal(i, (uint16_t)(i << 5));
    io16w(MOSAIC, MOSAIC_BG(4, 1));
    agb_ppu_render_frame();

    for (int x = 0; x < 4; x++)
        CHECK(px(x, 0) == argb(index_colour(1)), "affine column %d did not repeat column 0", x);
    for (int x = 4; x < 8; x++)
        CHECK(px(x, 0) == argb(index_colour(5)), "affine column %d did not repeat column 4", x);
}

static void test_object_vertical(void)
{
    TEST_CASE("object vertical mosaic");
    memset(&agb_mem, 0, sizeof(agb_mem));
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    add_striped_object(ATTR0_MOSAIC);
    io16w(MOSAIC, MOSAIC_OBJ(1, 4));
    agb_ppu_render_frame();

    for (int i = 0; i < 4; i++)
        CHECK(px(100, 50 + i) == argb(index_colour(1)),
              "object row %d did not repeat row 0", i);
    CHECK(px(100, 54) == argb(index_colour(5)), "object row 4 did not start a new block");
}

static void test_affine_background_vertical(void)
{
    TEST_CASE("affine bg vertical mosaic");
    memset(&agb_mem, 0, sizeof(agb_mem));

    io16w(DISPCNT, 1 | DISPCNT_BG2);
    io16w(BG2CNT, BGCNT_SCREEN_BASE(1) | BGCNT_MOSAIC);
    io16w(BG2PA, ONE);
    io16w(BG2PA + 6, ONE);
    io32w(BG2PA + 8, 0);
    io32w(BG2PA + 12, 0);
    for (int y = 0; y < 8; y++)
        agb_mem.vram[y * 8] = (uint8_t)(y + 1);
    for (int i = 1; i <= 8; i++)
        bg_pal(i, (uint16_t)(i << 5));
    io16w(MOSAIC, MOSAIC_BG(1, 4));
    agb_ppu_render_frame();

    for (int y = 0; y < 4; y++)
        CHECK(px(0, y) == argb(index_colour(1)), "affine row %d did not repeat row 0", y);
    CHECK(px(0, 4) == argb(index_colour(5)), "affine row 4 did not start a new block");
}

int main(void)
{
    test_background_horizontal();
    test_background_vertical();
    test_background_enable_bit();
    test_size_one_is_identity();
    test_background_and_object_sizes_are_separate();
    test_object_horizontal();
    test_object_enable_bit();
    test_object_ignores_background_sizes();
    test_object_vertical();
    test_affine_background();
    test_affine_background_vertical();

    return test_report("ppu mosaic");
}
