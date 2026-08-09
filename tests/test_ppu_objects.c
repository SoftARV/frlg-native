// Object-layer rendering, driven through OAM and VRAM directly.
//
// A running frame only exercises the paths the intro and title screen happen to
// use, which leaves 8bpp, 2D mapping, the size table and the wrap rules unproven.
//
// The register and memory layout is spelled out here rather than shared with
// platform/agb/src/ppu.c: a test that imports the renderer's own constants
// cannot catch a wrong one, because both sides would move together.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define OBJ_VRAM 0x10000
#define OBJ_PLTT 0x200

#define DISPCNT 0x000
#define BG0CNT 0x008

#define DISPCNT_BG0 0x0100
#define DISPCNT_1D 0x0040
#define DISPCNT_OBJ 0x1000

#define ATTR0_8BPP 0x2000
#define ATTR0_HIDDEN (2 << 8)
#define ATTR0_OBJ_WINDOW (2 << 10)
#define ATTR0_SHAPE_TALL (2 << 14)

#define ATTR1_HFLIP 0x1000
#define ATTR1_VFLIP 0x2000
#define ATTR1_SIZE(n) ((n) << 14)

#define ATTR0_AFFINE (1 << 8)
#define ATTR0_AFFINE_DOUBLE (3 << 8)
#define ATTR1_AFFINE_GROUP(n) ((n) << 9)

#define ATTR2_PRIORITY(n) ((n) << 10)
#define ATTR2_PALETTE(n) ((n) << 12)

#define ONE 0x100 // 1.0 in the matrix's 8.8 fixed point

#define RED 0x001F
#define GREEN 0x03E0
#define BLUE 0x7C00
#define WHITE 0x7FFF

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

static void reset(const char *name)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));
}

static void io16w(int offset, uint16_t value)
{
    *(volatile uint16_t *)(agb_mem.io + offset) = value;
}

static void oam_write(int n, uint16_t attr0, uint16_t attr1, uint16_t attr2)
{
    volatile uint16_t *entry = (volatile uint16_t *)(agb_mem.oam + n * 8);

    entry[0] = attr0;
    entry[1] = attr1;
    entry[2] = attr2;
}

// Matrix group g is interleaved into the fourth halfword of OAM entries 4g..4g+3.
static void oam_affine(int group, int pa, int pb, int pc, int pd)
{
    const int p[4] = {pa, pb, pc, pd};

    for (int i = 0; i < 4; i++)
        *(volatile uint16_t *)(agb_mem.oam + ((group * 4 + i) * 8) + 6) = (uint16_t)p[i];
}

static void obj_pal(int index, uint16_t colour)
{
    *(volatile uint16_t *)(agb_mem.pltt + OBJ_PLTT + index * 2) = colour;
}

static void bg_pal(int index, uint16_t colour)
{
    *(volatile uint16_t *)(agb_mem.pltt + index * 2) = colour;
}

// Set (x,y) inside the 8x8 tile that starts at 32-byte object slot `slot`.
static void tile4(int slot, int x, int y, int value)
{
    uint8_t *p = agb_mem.vram + OBJ_VRAM + slot * 32 + y * 4 + (x >> 1);

    *p = (x & 1) ? ((*p & 0x0F) | (value << 4)) : ((*p & 0xF0) | value);
}

static void tile8(int slot, int x, int y, int value)
{
    agb_mem.vram[OBJ_VRAM + slot * 32 + y * 8 + x] = value;
}

static uint32_t px(int x, int y)
{
    return agb_ppu_framebuffer()[y * agb_ppu_width() + x];
}

static void test_placement(void)
{
    reset("4bpp 8x8");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(3 * 16 + 5, RED);
    tile4(1, 2, 3, 5);
    oam_write(0, 50, 100, 1 | ATTR2_PALETTE(3));
    agb_ppu_render_frame();

    CHECK(px(102, 53) == argb(RED), "expected the pixel at (102,53), got %06X", px(102, 53));
    CHECK(px(101, 53) == argb(0), "colour index 0 was not transparent");
    CHECK(px(102, 52) == argb(0), "bled into the row above");
    CHECK(px(102, 53 + 8) == argb(0), "drew past the bottom of an 8x8 object");
}

static void test_flips(void)
{
    reset("hflip");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, RED);
    tile4(1, 2, 3, 5);
    oam_write(0, 50, 100 | ATTR1_HFLIP, 1);
    agb_ppu_render_frame();
    CHECK(px(105, 53) == argb(RED), "expected x=105 after hflip");
    CHECK(px(102, 53) == argb(0), "still drawn unflipped");

    reset("vflip");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, RED);
    tile4(1, 2, 3, 5);
    oam_write(0, 50, 100 | ATTR1_VFLIP, 1);
    agb_ppu_render_frame();
    CHECK(px(102, 54) == argb(RED), "expected y=54 after vflip");
    CHECK(px(102, 53) == argb(0), "still drawn unflipped");
}

// The bottom-right cell of a 16x16 object reaches a different tile under each
// mapping mode, which is the whole difference between them.
static void test_tile_mapping(void)
{
    reset("16x16 4bpp, 1D mapping");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, GREEN);
    tile4(4, 0, 0, 5); // tiles laid out back to back: base 1, cells 1,2,3,4
    oam_write(0, 50, 100 | ATTR1_SIZE(1), 1);
    agb_ppu_render_frame();
    CHECK(px(108, 58) == argb(GREEN), "1D: bottom-right cell misplaced");

    reset("16x16 4bpp, 2D mapping");
    io16w(DISPCNT, DISPCNT_OBJ);
    obj_pal(5, GREEN);
    tile4(1 + 32 + 1, 0, 0, 5); // one row down the 32-tile grid, one across
    oam_write(0, 50, 100 | ATTR1_SIZE(1), 1);
    agb_ppu_render_frame();
    CHECK(px(108, 58) == argb(GREEN), "2D: bottom-right cell misplaced");

    reset("16x16 8bpp, 1D mapping");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(200, BLUE);
    tile8(8, 0, 0, 200); // base slot 2, 64 bytes per cell -> 4th cell at slot 8
    oam_write(0, 50 | ATTR0_8BPP, 100 | ATTR1_SIZE(1), 2);
    agb_ppu_render_frame();
    CHECK(px(108, 58) == argb(BLUE), "8bpp 1D: bottom-right cell misplaced");

    reset("16x16 8bpp, 2D mapping");
    io16w(DISPCNT, DISPCNT_OBJ);
    obj_pal(200, BLUE);
    tile8(2 + 32 + 2, 0, 0, 200); // 1024-byte row stride, 64-byte column stride
    oam_write(0, 50 | ATTR0_8BPP, 100 | ATTR1_SIZE(1), 2);
    agb_ppu_render_frame();
    CHECK(px(108, 58) == argb(BLUE), "8bpp 2D: bottom-right cell misplaced");
}

static void test_size_table(void)
{
    reset("8x32 vertical");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, WHITE);
    tile4(4, 0, 7, 5); // fourth cell down, its last row
    oam_write(0, 50 | ATTR0_SHAPE_TALL, 100 | ATTR1_SIZE(1), 1);
    agb_ppu_render_frame();
    CHECK(px(100, 81) == argb(WHITE), "8x32: bottom row misplaced");
    CHECK(px(108, 81) == argb(0), "8x32: drew wider than 8 pixels");
}

static void test_object_ordering(void)
{
    reset("priority outranks OAM index");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, RED);
    obj_pal(6, GREEN);
    tile4(1, 0, 0, 5);
    tile4(2, 0, 0, 6);
    oam_write(0, 50, 100, 1 | ATTR2_PRIORITY(3));
    oam_write(1, 50, 100, 2 | ATTR2_PRIORITY(0));
    agb_ppu_render_frame();
    CHECK(px(100, 50) == argb(GREEN), "the higher-priority object did not win");

    reset("OAM index breaks a priority tie");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, RED);
    obj_pal(6, GREEN);
    tile4(1, 0, 0, 5);
    tile4(2, 0, 0, 6);
    oam_write(0, 50, 100, 1 | ATTR2_PRIORITY(1));
    oam_write(1, 50, 100, 2 | ATTR2_PRIORITY(1));
    agb_ppu_render_frame();
    CHECK(px(100, 50) == argb(RED), "the lower OAM index did not win the tie");
}

// An object sits above a background of equal priority, and below a stronger one.
static void test_object_against_background(void)
{
    for (int prio = 0; prio < 4; prio++)
    {
        const int bg_prio = 1;

        reset("object against background");
        io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_OBJ | DISPCNT_1D);
        io16w(BG0CNT, (1 << 8) | bg_prio); // char base 0, screen base 1
        memset(agb_mem.vram, 0x11, 32);    // tile 0: every pixel colour index 1
        bg_pal(1, BLUE);
        obj_pal(5, RED);
        tile4(1, 0, 0, 5);
        oam_write(0, 50, 100, 1 | ATTR2_PRIORITY(prio));
        agb_ppu_render_frame();

        CHECK(px(100, 50) == argb(prio <= bg_prio ? RED : BLUE),
              "object priority %d against background priority %d: wrong layer won",
              prio, bg_prio);
        CHECK(px(0, 0) == argb(BLUE), "the background did not cover the rest");
    }
}

static void test_edge_wrapping(void)
{
    reset("vertical wrap at 256");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, RED);
    tile4(3, 0, 0, 5); // object row 8, column 0
    oam_write(0, 250, 100 | ATTR1_SIZE(1), 1);
    agb_ppu_render_frame();
    CHECK(px(100, 2) == argb(RED), "an object placed at y=250 did not wrap to the top");

    reset("negative x clips instead of wrapping");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, RED);
    obj_pal(6, GREEN);
    tile4(2, 0, 0, 5); // the top-right cell of a 16x16
    tile4(1, 0, 0, 6); // the top-left cell, entirely off-screen
    oam_write(0, 50, 0x1F8 | ATTR1_SIZE(1), 1); // x = -8
    agb_ppu_render_frame();
    CHECK(px(0, 50) == argb(RED), "the on-screen half did not land at x=0");
    CHECK(px(232, 50) == argb(0), "off-screen columns wrapped around the right edge");
}

static void test_skipped_modes(void)
{
    reset("hidden objects are not drawn");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    // A usable matrix and opaque texels, so that mistaking mode 2 for one of the
    // affine modes draws something instead of quietly sampling a zero.
    oam_affine(0, ONE, 0, 0, ONE);
    memset(agb_mem.vram + OBJ_VRAM, 0x55, 0x100);
    obj_pal(5, RED);
    oam_write(0, 50 | ATTR0_HIDDEN, 100, 1);
    agb_ppu_render_frame();
    CHECK(px(100, 50) == argb(0), "a hidden object was drawn");
    CHECK(px(103, 53) == argb(0), "a hidden object was drawn through the affine path");

    reset("window objects contribute no pixels");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(5, RED);
    tile4(1, 0, 0, 5);
    oam_write(0, 50 | ATTR0_OBJ_WINDOW, 100, 1);
    agb_ppu_render_frame();
    CHECK(px(100, 50) == argb(0), "an OBJ-window object was drawn as pixels");

    reset("the object layer honours its enable bit");
    io16w(DISPCNT, DISPCNT_1D);
    obj_pal(5, RED);
    tile4(1, 0, 0, 5);
    oam_write(0, 50, 100, 1);
    agb_ppu_render_frame();
    CHECK(px(100, 50) == argb(0), "objects drawn with DISPCNT bit 12 clear");
}

// The identity matrix must put an affine object exactly where the same object
// would land with no transform at all — the check that anchors every other one.
static void test_affine_identity(void)
{
    reset("affine identity");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    oam_affine(0, ONE, 0, 0, ONE);
    obj_pal(5, RED);
    tile4(1, 2, 3, 5);
    oam_write(0, 50 | ATTR0_AFFINE, 100, 1);
    agb_ppu_render_frame();

    CHECK(px(102, 53) == argb(RED), "identity moved the object: got %06X at (102,53)", px(102, 53));
    CHECK(px(101, 53) == argb(0), "identity was not transparent where the texel is 0");

    reset("affine double size centres the object");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    oam_affine(0, ONE, 0, 0, ONE);
    obj_pal(5, RED);
    tile4(1, 2, 3, 5);
    oam_write(0, 50 | ATTR0_AFFINE_DOUBLE, 100, 1);
    agb_ppu_render_frame();

    // The 8x8 object sits in the middle of a 16x16 box, so every pixel shifts
    // by half the object's size.
    CHECK(px(106, 57) == argb(RED), "double size did not centre the object in its box");
    CHECK(px(102, 53) == argb(0), "double size drew at the single-size position");
}

// Rotation by 90 degrees: tx follows dy, ty follows -dx.
static void test_affine_rotation(void)
{
    reset("affine 90 degree rotation");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    oam_affine(0, 0, ONE, -ONE, 0);
    obj_pal(5, GREEN);
    tile4(1, 6, 7, 5); // texel (6,7) of a 16x16 falls in the first cell
    oam_write(0, 50 | ATTR0_AFFINE, 100 | ATTR1_SIZE(1), 1);
    agb_ppu_render_frame();

    CHECK(px(109, 56) == argb(GREEN), "rotated texel did not land at (109,56)");
    // Where the identity transform would have put it — proves a rotation ran.
    CHECK(px(106, 57) == argb(0), "texel landed at its unrotated position");
}

// Doubling the matrix halves the object: two texels per screen pixel.
static void test_affine_scale(void)
{
    reset("affine scale down");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    oam_affine(0, 2 * ONE, 0, 0, 2 * ONE);
    obj_pal(5, BLUE);
    tile4(1 + 3, 4, 0, 5); // texel (12,8) of a 16x16: fourth cell, local (4,0)
    oam_write(0, 50 | ATTR0_AFFINE, 100 | ATTR1_SIZE(1), 1);
    agb_ppu_render_frame();

    CHECK(px(110, 58) == argb(BLUE), "scaled texel did not land at (110,58)");
}

// A source coordinate outside the object must clip, not sample whatever tile
// data happens to sit there. The object region is filled first, so an unclipped
// read returns a real colour rather than passing by landing on zeroes -- which
// is exactly how an earlier version of this check fooled itself.
static void test_affine_source_bounds(void)
{
    reset("affine source coordinate bounds");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    oam_affine(0, 2 * ONE, 0, 0, 2 * ONE);
    memset(agb_mem.vram + OBJ_VRAM, 0x33, 0x8000); // every texel is colour 3
    obj_pal(3, WHITE);
    oam_write(0, 50 | ATTR0_AFFINE, 100 | ATTR1_SIZE(1), 1);
    agb_ppu_render_frame();

    // At 2x, screen columns 0-3 and 12-15 map outside the object's 16 pixels,
    // and box rows 12-15 do the same vertically.
    CHECK(px(100, 58) == argb(0), "sampled past the left edge of the object");
    CHECK(px(103, 58) == argb(0), "sampled past the left edge of the object");
    CHECK(px(104, 58) == argb(WHITE), "clipped a column that is inside the object");
    CHECK(px(111, 58) == argb(WHITE), "clipped a column that is inside the object");
    CHECK(px(112, 58) == argb(0), "sampled past the right edge of the object");
    CHECK(px(115, 58) == argb(0), "sampled past the right edge of the object");
    CHECK(px(108, 62) == argb(0), "sampled past the bottom edge of the object");
}

// The selector is 5 bits, and its top two are where a regular object keeps its
// flip flags — so an affine object must read a matrix, never a flip.
static void test_affine_group_selection(void)
{
    reset("affine group selector");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    oam_affine(0, 0, 0, 0, 0); // group 0 left degenerate on purpose
    oam_affine(3, ONE, 0, 0, ONE);
    obj_pal(5, RED);
    tile4(1, 2, 3, 5);
    oam_write(0, 50 | ATTR0_AFFINE, 100 | ATTR1_AFFINE_GROUP(3), 1);
    agb_ppu_render_frame();

    CHECK(px(102, 53) == argb(RED), "group 3 was not the matrix that got used");
    CHECK(px(101, 53) == argb(0), "a degenerate matrix flooded the box");

    reset("flip bits are group bits on an affine object");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    oam_affine(24, ONE, 0, 0, ONE); // 0x1000 | 0x2000 selects group 8 | 16
    obj_pal(5, RED);
    tile4(1, 2, 3, 5);
    oam_write(0, 50 | ATTR0_AFFINE, 100 | ATTR1_HFLIP | ATTR1_VFLIP, 1);
    agb_ppu_render_frame();

    CHECK(px(102, 53) == argb(RED), "the flip bits were treated as flips, not as group 24");
    CHECK(px(105, 54) == argb(0), "the object was flipped");
}

// The largest object at the last tile slot addresses past the object region.
// Nothing here can assert where it landed, so this case earns its keep only
// under a sanitizer. No preset configures one: build this file by hand with
// -fsanitize=address,undefined over platform/agb/src/{ppu,memmap}.c.
static void test_tile_index_bound(void)
{
    reset("out-of-range tile index stays in the object region");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
    obj_pal(255, RED);
    oam_write(0, 50 | ATTR0_8BPP, 100 | ATTR1_SIZE(3), 1023);
    agb_ppu_render_frame();
}

int main(void)
{
    test_placement();
    test_flips();
    test_tile_mapping();
    test_size_table();
    test_object_ordering();
    test_object_against_background();
    test_edge_wrapping();
    test_skipped_modes();
    test_affine_identity();
    test_affine_rotation();
    test_affine_scale();
    test_affine_source_bounds();
    test_affine_group_selection();
    test_tile_index_bound();

    return test_report("ppu objects");
}
