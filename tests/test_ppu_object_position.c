// An object placed from what the game meant, rather than from what OAM kept.
//
// OAM holds nine bits of an object's X and eight of its Y, and the hardware
// wraps both. At 240x160 that is invisible: everything outside the screen is
// outside it either way. A taller viewport is not so lucky -- its top edge is
// 256 pixels above the ground the field spawns objects on, and 256 is where the
// coordinate comes round again, so NPCs standing below the screen were drawn
// along the top of it.
//
// ADR 0024: the truncation is the encoding's, not the game's, which computes
// these as a signed pair. Given that pair, there is nothing to come round.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define OBJ_VRAM 0x10000
#define OBJ_PLTT 0x200

#define DISPCNT 0x000
#define DISPCNT_OBJ 0x1000
#define DISPCNT_1D 0x0040
#define ATTR0_8BPP 0x2000

#define GREEN 0x03E0

// An object below a 240-tall view: far enough down that the hardware's own wrap
// brings it back to the top.
#define BELOW_THE_VIEW 216
#define OBJ_LEFT 100

static void io16(int offset, uint16_t value)
{
    agb_mem.io[offset] = (uint8_t)value;
    agb_mem.io[offset + 1] = (uint8_t)(value >> 8);
}

static void oam16(int offset, uint16_t value)
{
    agb_mem.oam[offset] = (uint8_t)value;
    agb_mem.oam[offset + 1] = (uint8_t)(value >> 8);
}

static void build_scene(void)
{
    memset(&agb_mem, 0, sizeof(agb_mem));
    agb_ppu_clear_object_positions();

    // The backdrop stays black, so a pixel that is not black is the object and
    // nothing else. Painting it white made every one of these tests pass on the
    // backdrop instead.
    agb_mem.pltt[OBJ_PLTT + 2] = GREEN & 0xFF;
    agb_mem.pltt[OBJ_PLTT + 3] = GREEN >> 8;

    // One 8bpp 32x32 object, every texel opaque.
    for (int i = 0; i < 64 * 16; i++)
        agb_mem.vram[OBJ_VRAM + i] = 1;

    oam16(0, BELOW_THE_VIEW | ATTR0_8BPP);
    oam16(2, OBJ_LEFT | (3 << 14));   // size 3 of the square shapes: 32x32
    oam16(4, 0);
    io16(DISPCNT, DISPCNT_OBJ | DISPCNT_1D);
}

// Anything drawn at all, anywhere along the top edge.
static int drawn_near_the_top(void)
{
    const uint32_t *frame = agb_ppu_framebuffer();
    int w = agb_ppu_width();
    int count = 0;

    for (int y = 0; y < 40; y++)
    {
        for (int x = 0; x < w; x++)
        {
            if (frame[y * w + x] != 0)
                count++;
        }
    }
    return count;
}

static void test_the_wrap_is_what_it_looks_like(void)
{
    TEST_CASE("without the game's own position, an object below the view comes round to the top");

    agb_ppu_set_viewport(AGB_PPU_MIN_W, 240);
    build_scene();
    agb_ppu_render_frame();

    // Not a demand on the port so much as a record of why the next case exists:
    // this is the hardware's wrap, seen through a viewport tall enough to reach
    // it. If this ever stops happening the fault below has gone another way.
    CHECK(drawn_near_the_top() > 0,
          "the object at y=%d did not wrap, so this test no longer covers anything",
          BELOW_THE_VIEW);
}

static void test_the_game_s_own_position_does_not_wrap(void)
{
    TEST_CASE("given where the game put it, an object below the view stays below it");

    agb_ppu_set_viewport(AGB_PPU_MIN_W, 240);
    build_scene();
    agb_ppu_set_object_position(0, OBJ_LEFT, BELOW_THE_VIEW);
    agb_ppu_render_frame();

    CHECK(drawn_near_the_top() == 0,
          "%d pixels of an object standing below the screen were drawn along the top of it",
          drawn_near_the_top());
}

static void test_it_still_draws_where_it_should(void)
{
    const uint32_t *frame;
    int w;

    TEST_CASE("and an object inside the view is drawn where the game put it");

    agb_ppu_set_viewport(AGB_PPU_MIN_W, 240);
    build_scene();
    oam16(0, 100 | ATTR0_8BPP);
    agb_ppu_set_object_position(0, OBJ_LEFT, 100);
    agb_ppu_render_frame();

    frame = agb_ppu_framebuffer();
    w = agb_ppu_width();
    // The viewport is 80 taller than the hardware's, so its own y=100 is 40
    // further down the picture.
    CHECK(frame[(100 + 40) * w + OBJ_LEFT] != 0, "nothing was drawn where the object is");
    CHECK(frame[(100 + 40 - 8) * w + OBJ_LEFT] == 0, "something was drawn above it");
}

int main(void)
{
    test_the_wrap_is_what_it_looks_like();
    test_the_game_s_own_position_does_not_wrap();
    test_it_still_draws_where_it_should();
    agb_ppu_clear_object_positions();
    agb_ppu_set_viewport(AGB_PPU_MIN_W, AGB_PPU_MIN_H);
    return test_report("ppu_object_position");
}
