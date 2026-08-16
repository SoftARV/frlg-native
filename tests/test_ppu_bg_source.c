// A background read from the game's own buffer instead of from VRAM (ADR 0024).
//
// Two things have to hold, and they pull in opposite directions. Handing over a
// copy of what VRAM already holds must change nothing at all -- that is what
// makes the feature safe to switch on for one background and not the others.
// And handing over a bigger one must actually be bigger, wrapping where the
// buffer ends rather than where a register's four sizes would have.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define DISPCNT 0x000
#define BG0CNT 0x008
#define BG0HOFS 0x010
#define BG0VOFS 0x012

#define DISPCNT_BG0 0x0100
#define SCREEN_BASE 31
#define WHITE 0x7FFF
#define RED 0x001F
#define GREEN 0x03E0

static uint32_t vram_frame[AGB_PPU_MIN_W * AGB_PPU_MIN_H];
static uint16_t handed[96 * 64];

static void io16(int offset, uint16_t value)
{
    agb_mem.io[offset] = (uint8_t)value;
    agb_mem.io[offset + 1] = (uint8_t)(value >> 8);
}

static void pltt16(int offset, uint16_t value)
{
    agb_mem.pltt[offset] = (uint8_t)value;
    agb_mem.pltt[offset + 1] = (uint8_t)(value >> 8);
}

// Two tiles that are solid and different, so which entry was fetched is legible
// from any single pixel.
static void build_tiles(void)
{
    memset(&agb_mem, 0, sizeof(agb_mem));
    pltt16(0, WHITE);
    pltt16(2, RED);
    pltt16(4, GREEN);

    for (int i = 0; i < 32; i++)
    {
        agb_mem.vram[i] = 0x11;       // tile 0: palette entry 1
        agb_mem.vram[32 + i] = 0x22;  // tile 1: palette entry 2
    }

    io16(BG0CNT, (SCREEN_BASE << 8));
    io16(BG0HOFS, 0);
    io16(BG0VOFS, 0);
    io16(DISPCNT, DISPCNT_BG0);
}

// The same diagonal in VRAM's 32x32 screen block and in a plain array.
static void fill_both(int width, int height)
{
    for (int ty = 0; ty < height; ty++)
    {
        for (int tx = 0; tx < width; tx++)
        {
            uint16_t entry = ((tx + ty) & 3) == 0 ? 0 : 1;

            handed[ty * width + tx] = entry;
            if (tx < 32 && ty < 32)
                agb_mem.vram[SCREEN_BASE * 0x800 + (ty * 32 + tx) * 2] = (uint8_t)entry;
        }
    }
}

static void test_handing_over_a_copy_changes_nothing(void)
{
    const uint32_t *frame;
    int wrong = 0;

    TEST_CASE("a buffer holding what VRAM holds renders identically");

    agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
    build_tiles();
    fill_both(32, 32);

    agb_ppu_set_bg_source(0, NULL, 0, 0);
    agb_ppu_render_frame();
    memcpy(vram_frame, agb_ppu_framebuffer(), sizeof(vram_frame));

    agb_ppu_set_bg_source(0, handed, 32, 32);
    agb_ppu_set_bg_scroll(0, 0, 0);
    agb_ppu_render_frame();
    frame = agb_ppu_framebuffer();

    for (int i = 0; i < AGB_PPU_MIN_W * AGB_PPU_MIN_H; i++)
    {
        if (frame[i] != vram_frame[i])
            wrong++;
    }
    CHECK(wrong == 0, "%d pixels differ between VRAM and the same map handed over", wrong);
}

static void test_a_bigger_buffer_wraps_where_it_ends(void)
{
    const uint32_t *frame;
    uint32_t red, green;

    TEST_CASE("a buffer larger than any of the register's sizes is read at its own size");

    agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
    build_tiles();

    // Every tile is 0 except one column at x = 40, which no 32-wide background
    // could show: it would wrap to x = 8 and be indistinguishable from the map
    // repeating. At 64 wide it is 320 pixels across, reachable only by scrolling.
    for (int i = 0; i < 64 * 64; i++)
        handed[i] = 0;
    for (int ty = 0; ty < 64; ty++)
        handed[ty * 64 + 40] = 1;

    agb_ppu_set_bg_source(0, handed, 64, 64);
    // Scrolled through the port rather than the register: nine bits cannot say
    // 320 on a background that is 512 wide and could not say it at all on one
    // wider. A handed-over background takes its scroll the same way it takes
    // its size.
    agb_ppu_set_bg_scroll(0, 320 - 8, 0);
    agb_ppu_render_frame();
    frame = agb_ppu_framebuffer();

    red = frame[0];
    green = frame[8];
    CHECK(red != green, "the column at tile 40 was not fetched: the buffer wrapped early");

    // And the wrap is at 64 tiles, not beyond: 512 pixels further on is the same
    // column again.
    agb_ppu_set_bg_scroll(0, 320 - 8 + 512, 0);
    agb_ppu_render_frame();
    CHECK(agb_ppu_framebuffer()[8] == green, "the buffer did not wrap at its own width");
}

static void test_letting_go_returns_it_to_vram(void)
{
    TEST_CASE("clearing the source puts the background back on VRAM");

    agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
    build_tiles();
    fill_both(32, 32);

    agb_ppu_set_bg_source(0, handed, 32, 32);
    agb_ppu_set_bg_scroll(0, 0, 0);
    agb_ppu_render_frame();
    memcpy(vram_frame, agb_ppu_framebuffer(), sizeof(vram_frame));

    // The buffer says one thing and VRAM another; after letting go it must be
    // VRAM that is believed.
    for (int i = 0; i < 32 * 32; i++)
        handed[i] = 1;
    agb_ppu_set_bg_source(0, NULL, 0, 0);
    agb_ppu_render_frame();

    CHECK(memcmp(agb_ppu_framebuffer(), vram_frame, sizeof(vram_frame)) == 0,
          "the frame did not go back to what VRAM holds");
}

// Every column of a wide buffer must land where it should at the widest viewport
// there is. This is what caught the scanline buffers being sized to 512 while
// the viewport could be asked for more: every pixel past 512 was written off the
// end of an array, and the picture beyond it was whatever that landed on.
static void test_every_column_of_a_wide_buffer(void)
{
    const int W = 80, H = 64;
    int wrong = 0;

    TEST_CASE("every column of an 80-tile buffer lands where it should at 576 wide");

    agb_ppu_set_viewport(AGB_PPU_MAX_W, AGB_PPU_MIN_H);
    build_tiles();

    // Tile 1 down one column, tile 0 everywhere else -- moved to each column in
    // turn, so a column that is fetched from the wrong place shows up as the
    // marker appearing at the wrong screen x, or not at all.
    for (int col = 0; col < W; col++)
    {
        const uint32_t *frame;
        int scroll = 0;
        int expected;

        for (int i = 0; i < W * H; i++)
            handed[i] = 0;
        for (int ty = 0; ty < H; ty++)
            handed[ty * W + col] = 1;

        agb_ppu_set_bg_source(0, handed, W, H);
        agb_ppu_set_bg_scroll(0, scroll, 0);
        agb_ppu_render_frame();
        frame = agb_ppu_framebuffer();

        // screen x = buffer x - scroll + view_ox, where view_ox is (576-240)/2
        expected = col * 8 - scroll + (AGB_PPU_MAX_W - AGB_PPU_MIN_W) / 2;
        if (expected < 0 || expected + 8 > AGB_PPU_MAX_W)
            continue;
        if (frame[10 * AGB_PPU_MAX_W + expected + 4]
         == frame[10 * AGB_PPU_MAX_W + expected - 4])
            wrong++;
    }
    CHECK(wrong == 0, "%d of %d columns did not render where they belong", wrong, W);
}

int main(void)
{
    test_every_column_of_a_wide_buffer();
    test_handing_over_a_copy_changes_nothing();
    test_a_bigger_buffer_wraps_where_it_ends();
    test_letting_go_returns_it_to_vram();
    agb_ppu_set_bg_source(0, NULL, 0, 0);
    agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
    return test_report("ppu_bg_source");
}
