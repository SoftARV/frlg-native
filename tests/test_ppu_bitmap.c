// The bitmap modes: a frame buffer on BG2, transformed by BG2's matrix.
//
// The three differ in what a pixel is and how many frames fit: mode 3 is one
// full-screen 16-bit frame, mode 4 is 8-bit paletted with two, mode 5 is 16-bit
// with two but only 160x128 each.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define OBJ_VRAM 0x10000
#define OBJ_PLTT 0x200

#define DISPCNT 0x000
#define BG0CNT 0x008
#define BG2CNT 0x00C
#define BG2PA 0x020

#define DISPCNT_FRAME1 0x0010
#define DISPCNT_1D 0x0040
#define DISPCNT_BG0 0x0100
#define DISPCNT_BG2 0x0400
#define DISPCNT_OBJ 0x1000

#define BGCNT_SCREEN_BASE(n) ((n) << 8)

#define FRAME1 0xA000
#define MODE5_W 160
#define MODE5_H 128

#define ONE 0x100

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

static void put16(uint32_t byte_offset, uint16_t value)
{
    agb_mem.vram[byte_offset] = (uint8_t)(value & 0xFF);
    agb_mem.vram[byte_offset + 1] = (uint8_t)(value >> 8);
}

// The backdrop is given a colour of its own so that "the bitmap drew nothing
// here" is distinguishable from "the bitmap drew black here".
static void reset_bitmap(const char *name, int mode, uint16_t extra)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));

    io16w(DISPCNT, (uint16_t)(mode | DISPCNT_BG2 | extra));
    io16w(BG2CNT, 0);
    io16w(BG2PA, ONE);     // identity matrix
    io16w(BG2PA + 6, ONE);
    io32w(BG2PA + 8, 0);
    io32w(BG2PA + 12, 0);
    bg_pal(0, BLUE);
}

// Mode 3: one 16-bit frame filling the screen, every pixel of it opaque.
static void test_mode3(void)
{
    reset_bitmap("mode 3 direct colour", 3, 0);
    put16((2 * 240 + 3) * 2, RED);
    agb_ppu_render_frame();

    CHECK(px(3, 2) == argb(RED), "the frame buffer pixel did not land at (3,2)");
    // A zero in a direct-colour frame is black, not transparent: the backdrop
    // must not show through anywhere.
    CHECK(px(4, 2) == argb(0), "a zero pixel was treated as transparent");
    CHECK(px(239, 159) == argb(0), "the frame did not cover the whole screen");

    reset_bitmap("mode 3 has no second frame", 3, DISPCNT_FRAME1);
    put16((2 * 240 + 3) * 2, RED);
    put16(FRAME1 + (2 * 240 + 3) * 2, GREEN);
    agb_ppu_render_frame();
    CHECK(px(3, 2) == argb(RED), "mode 3 honoured the frame select bit");
}

// Mode 4: 8-bit paletted, and index 0 is transparent again.
static void test_mode4(void)
{
    reset_bitmap("mode 4 paletted", 4, 0);
    bg_pal(5, GREEN);
    agb_mem.vram[2 * 240 + 3] = 5;
    agb_ppu_render_frame();

    CHECK(px(3, 2) == argb(GREEN), "the paletted pixel did not land at (3,2)");

    // Palette entry 0 and the backdrop are the same register, so a transparent
    // pixel and an opaque index 0 are the same colour. Only something layered
    // underneath can tell them apart.
    TEST_CASE("mode 4 index 0 is transparent");
    memset(&agb_mem, 0, sizeof(agb_mem));
    io16w(DISPCNT, 4 | DISPCNT_BG2 | DISPCNT_OBJ | DISPCNT_1D);
    io16w(BG2CNT, 0); // priority 0, in front of the object
    io16w(BG2PA, ONE);
    io16w(BG2PA + 6, ONE);
    io32w(BG2PA + 8, 0);
    io32w(BG2PA + 12, 0);
    bg_pal(0, BLUE);
    bg_pal(5, WHITE);
    agb_mem.vram[2 * 240 + 3] = 5; // one opaque bitmap pixel
    memset(agb_mem.vram + OBJ_VRAM + 512 * 32, 0x55, 32);
    *(volatile uint16_t *)(agb_mem.pltt + OBJ_PLTT + 5 * 2) = GREEN;
    {
        volatile uint16_t *entry = (volatile uint16_t *)agb_mem.oam;

        entry[0] = 0;
        entry[1] = 0;
        entry[2] = (uint16_t)(512 | (1 << 10)); // priority 1, behind BG2
    }
    agb_ppu_render_frame();

    CHECK(px(3, 2) == argb(WHITE), "the opaque bitmap pixel did not cover the object");
    CHECK(px(4, 2) == argb(GREEN), "index 0 was opaque: the object under it did not show");

    reset_bitmap("mode 4 frame select", 4, DISPCNT_FRAME1);
    bg_pal(5, GREEN);
    bg_pal(6, RED);
    agb_mem.vram[2 * 240 + 3] = 6;          // frame 0
    agb_mem.vram[FRAME1 + 2 * 240 + 3] = 5; // frame 1
    agb_ppu_render_frame();
    CHECK(px(3, 2) == argb(GREEN), "the frame select bit did not reach the second frame");
}

// Mode 5: 16-bit again but only 160x128, so most of the screen is off the frame.
static void test_mode5(void)
{
    reset_bitmap("mode 5 extent", 5, 0);
    put16((2 * MODE5_W + 3) * 2, WHITE);
    agb_ppu_render_frame();

    CHECK(px(3, 2) == argb(WHITE), "the mode 5 pixel did not land at (3,2)");
    CHECK(px(MODE5_W, 2) == argb(BLUE), "drew past the right edge of a 160-wide frame");
    CHECK(px(3, MODE5_H) == argb(BLUE), "drew past the bottom of a 128-tall frame");
    CHECK(px(MODE5_W - 1, MODE5_H - 1) == argb(0), "did not cover the last pixel of the frame");

    reset_bitmap("mode 5 frame select", 5, DISPCNT_FRAME1);
    put16((2 * MODE5_W + 3) * 2, RED);
    put16(FRAME1 + (2 * MODE5_W + 3) * 2, GREEN);
    agb_ppu_render_frame();
    CHECK(px(3, 2) == argb(GREEN), "the frame select bit did not reach the second frame");
}

// The matrix transforms a frame buffer exactly as it does a tiled affine map.
static void test_transform(void)
{
    reset_bitmap("bitmap reference point", 3, 0);
    io32w(BG2PA + 8, 8 << 8); // start eight pixels into the frame
    put16((0 * 240 + 8) * 2, RED);
    agb_ppu_render_frame();
    CHECK(px(0, 0) == argb(RED), "the reference point did not shift the frame");

    reset_bitmap("bitmap scale", 3, 0);
    io16w(BG2PA, 2 * ONE); // two frame pixels per screen pixel
    put16((0 * 240 + 4) * 2, GREEN);
    agb_ppu_render_frame();
    CHECK(px(2, 0) == argb(GREEN), "the matrix did not scale the frame");

    reset_bitmap("bitmap does not wrap", 3, 0);
    io32w(BG2PA + 8, (uint32_t)(-(16 << 8)) & 0x0FFFFFFF);
    put16(0, RED); // the frame's own first pixel
    agb_ppu_render_frame();
    CHECK(px(0, 0) == argb(BLUE), "sampled off the left of the frame instead of clipping");
    CHECK(px(16, 0) == argb(RED), "the frame did not start where the reference point put it");
}

// A bitmap mode defines BG2 and nothing else, whatever DISPCNT enables.
static void test_only_bg2(void)
{
    // BG0 enabled and BG2 not. Correctly, nothing is drawn at all; a layer that
    // wrongly took the bitmap path would show the frame buffer instead. Leaving
    // BG2 on as well would let a mistake deposit the same colour and hide.
    TEST_CASE("bitmap modes define only BG2");
    memset(&agb_mem, 0, sizeof(agb_mem));
    io16w(DISPCNT, 3 | DISPCNT_BG0);
    io16w(BG0CNT, 0);
    io16w(BG2PA, ONE);
    io16w(BG2PA + 6, ONE);
    io32w(BG2PA + 8, 0);
    io32w(BG2PA + 12, 0);
    bg_pal(0, BLUE);
    put16(0, RED);
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(BLUE), "a background other than BG2 drew in a bitmap mode");
}

// Half the object tile region belongs to the frame buffer in these modes, so
// objects below tile 512 have nothing to draw from.
static void test_object_tile_floor(void)
{
    volatile uint16_t *entry = (volatile uint16_t *)agb_mem.oam;

    reset_bitmap("objects below tile 512", 3, DISPCNT_OBJ | DISPCNT_1D);
    memset(agb_mem.vram + OBJ_VRAM + 32, 0x55, 32); // tile 1
    *(volatile uint16_t *)(agb_mem.pltt + OBJ_PLTT + 5 * 2) = GREEN;
    entry[0] = 50;
    entry[1] = 100;
    entry[2] = 1;
    agb_ppu_render_frame();
    CHECK(px(102, 52) == argb(0), "an object below tile 512 was drawn in a bitmap mode");

    reset_bitmap("objects at tile 512 and above", 3, DISPCNT_OBJ | DISPCNT_1D);
    memset(agb_mem.vram + OBJ_VRAM + 512 * 32, 0x55, 32);
    *(volatile uint16_t *)(agb_mem.pltt + OBJ_PLTT + 5 * 2) = GREEN;
    entry[0] = 50;
    entry[1] = 100;
    entry[2] = 512;
    agb_ppu_render_frame();
    CHECK(px(102, 52) == argb(GREEN), "an object at tile 512 was not drawn");
}

int main(void)
{
    test_mode3();
    test_mode4();
    test_mode5();
    test_transform();
    test_only_bg2();
    test_object_tile_floor();

    return test_report("ppu bitmap modes");
}
