// The one invariant a wider viewport must never break: it shows *more*, never
// *different*.
//
// Whatever the game draws at the hardware's own size has to appear unchanged
// inside a larger frame, centred, with the extra area added around it. If a
// layer is anchored to the corner while the others are centred, the picture
// comes apart -- and it comes apart quietly, because every layer still renders
// and only their relationship is wrong.
//
// That is not hypothetical. Objects took their row straight from the viewport's
// scanline while their column was shifted, so every sprite sat 48 pixels out at
// a 256-tall viewport and the backgrounds were correct. It looked plausible on
// screen and was obvious the moment the two frames were compared.
//
// So the test is the comparison: render a scene at 240x160, render the same
// scene wider, and require the first to be a sub-image of the second.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define OBJ_VRAM 0x10000
#define OBJ_PLTT 0x200

#define DISPCNT 0x000
#define BG0CNT 0x008
#define BG0HOFS 0x010
#define BG0VOFS 0x012

#define DISPCNT_BG0 0x0100
#define DISPCNT_1D 0x0040
#define DISPCNT_OBJ 0x1000

#define ATTR0_8BPP 0x2000

#define WHITE 0x7FFF
#define RED 0x001F
#define GREEN 0x03E0

static uint32_t native[AGB_PPU_MIN_W * AGB_PPU_MIN_H];

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

static void oam16(int offset, uint16_t value)
{
    agb_mem.oam[offset] = (uint8_t)value;
    agb_mem.oam[offset + 1] = (uint8_t)(value >> 8);
}

// A background with a recognisable tile, and one object sitting on it. Both
// matter: the fault this test exists for was a background and an object
// disagreeing, which neither alone could have shown.
static void build_scene(void)
{
    memset(&agb_mem, 0, sizeof(agb_mem));

    pltt16(2, RED);
    pltt16(OBJ_PLTT + 2, GREEN);
    pltt16(0, WHITE);

    // One 4bpp tile, every texel palette entry 1.
    for (int i = 0; i < 32; i++)
        agb_mem.vram[i] = 0x11;
    // Tilemap at screen block 31: a diagonal, so a shift in either axis shows.
    for (int ty = 0; ty < 32; ty++)
        for (int tx = 0; tx < 32; tx++)
            if (((tx + ty) & 3) == 0)
                agb_mem.vram[0xF800 + (ty * 32 + tx) * 2] = 0;
            else
                agb_mem.vram[0xF800 + (ty * 32 + tx) * 2] = 1;

    // An 8bpp object tile, and one object placed away from the origin so a
    // missing offset in either axis moves it.
    for (int i = 0; i < 64; i++)
        agb_mem.vram[OBJ_VRAM + i] = 1;
    oam16(0, 60 | ATTR0_8BPP);   // y = 60
    oam16(2, 100);               // x = 100
    oam16(4, 0);

    io16(BG0CNT, (31 << 8));
    io16(BG0HOFS, 3);
    io16(BG0VOFS, 5);
    io16(DISPCNT, DISPCNT_BG0 | DISPCNT_OBJ | DISPCNT_1D);
}

static void render_native(void)
{
    agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
    build_scene();
    agb_ppu_render_frame();
    memcpy(native, agb_ppu_framebuffer(), sizeof(native));
}

// The whole point, in one function: the native frame must appear inside the
// wider one, unchanged and centred.
static int contains_native(int width, int height)
{
    const uint32_t *wide;
    int ox = (width - AGB_PPU_MIN_W) / 2;
    int oy = (height - AGB_PPU_MIN_H) / 2;
    int wrong = 0;

    agb_ppu_set_viewport(width, height);
    build_scene();
    agb_ppu_render_frame();
    wide = agb_ppu_framebuffer();

    for (int y = 0; y < AGB_PPU_MIN_H; y++)
    {
        for (int x = 0; x < AGB_PPU_MIN_W; x++)
        {
            if (wide[(y + oy) * width + (x + ox)] != native[y * AGB_PPU_MIN_W + x])
                wrong++;
        }
    }
    return wrong;
}

static void test_wider_contains_native(void)
{
    TEST_CASE("a wider viewport shows more, not different");
    render_native();

    CHECK(contains_native(256, 160) == 0,
          "256x160 does not contain the native frame unchanged");
}

static void test_taller_contains_native(void)
{
    TEST_CASE("a taller viewport shows more, not different");
    render_native();

    // The axis the object fault was on: a sprite's row came from the viewport's
    // scanline rather than the hardware's, so this is the case that failed.
    CHECK(contains_native(240, 256) == 0,
          "240x256 does not contain the native frame unchanged");
}

static void test_both_axes(void)
{
    TEST_CASE("wider and taller at once");
    render_native();

    CHECK(contains_native(256, 256) == 0,
          "256x256 does not contain the native frame unchanged");
}

static void test_odd_sizes(void)
{
    TEST_CASE("a viewport that is not evenly larger still centres");
    render_native();

    // An odd difference cannot be split evenly, and the renderer and this test
    // have to round it the same way or everything lands a pixel out.
    CHECK(contains_native(247, 199) == 0,
          "247x199 does not contain the native frame unchanged");
}

static void test_clamped_to_what_the_game_draws(void)
{
    TEST_CASE("the viewport is clamped, not taken on trust");
    agb_ppu_set_viewport(4000, 4000);
    CHECK(agb_ppu_width() == AGB_PPU_MAX_W, "width became %d", agb_ppu_width());
    CHECK(agb_ppu_height() == AGB_PPU_MAX_H, "height became %d", agb_ppu_height());

    agb_ppu_set_viewport(1, 1);
    CHECK(agb_ppu_width() == AGB_PPU_MIN_W, "width became %d", agb_ppu_width());
    CHECK(agb_ppu_height() == AGB_PPU_MIN_H, "height became %d", agb_ppu_height());

    TEST_CASE("setting the same viewport twice reports no change");
    agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
    CHECK(!agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H),
          "an unchanged viewport reported a change");
}

int main(void)
{
    test_wider_contains_native();
    test_taller_contains_native();
    test_both_axes();
    test_odd_sizes();
    test_clamped_to_what_the_game_draws();
    agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
    return test_report("ppu_viewport");
}
