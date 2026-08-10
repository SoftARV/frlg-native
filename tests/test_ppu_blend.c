// Colour special effects: alpha blending, the two brightness effects, and the
// rules deciding when any of them apply.
//
// Expected colours are worked out by hand and written as literals rather than
// computed here, so the test cannot agree with the renderer by sharing its
// arithmetic.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define OBJ_VRAM 0x10000
#define OBJ_PLTT 0x200
#define SCREEN_BLOCK 0x800

#define DISPCNT 0x000
#define BG0CNT 0x008
#define BG1CNT 0x00A
#define WIN0H 0x040
#define WIN0V 0x044
#define WININ 0x048
#define WINOUT 0x04A
#define BLDCNT 0x050
#define BLDALPHA 0x052
#define BLDY 0x054

#define DISPCNT_BG0 0x0100
#define DISPCNT_BG1 0x0200
#define DISPCNT_OBJ 0x1000
#define DISPCNT_1D 0x0040
#define DISPCNT_WIN0 0x2000

#define BGCNT_SCREEN_BASE(n) ((n) << 8)

// Blend targets, in the order BLDCNT names them.
#define TARGET_BG0 0x01
#define TARGET_BG1 0x02
#define TARGET_OBJ 0x10
#define TARGET_BACKDROP 0x20
#define SECOND(t) ((t) << 8)

#define EFFECT_ALPHA (1 << 6)
#define EFFECT_BRIGHTEN (2 << 6)
#define EFFECT_DARKEN (3 << 6)

#define WIN_BG0 0x01
#define WIN_BG1 0x02
#define WIN_OBJ 0x10
#define WIN_EFFECT 0x20

#define ATTR0_SEMI (1 << 10)

#define BGR(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

// The two operands every test blends, chosen so each channel is easy to follow:
// the top layer is red only, the layer under it green only.
#define TOP BGR(16, 0, 0)
#define UNDER BGR(0, 8, 0)

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

static void bg_pal(int index, uint16_t colour)
{
    *(volatile uint16_t *)(agb_mem.pltt + index * 2) = colour;
}

static uint32_t px(int x, int y)
{
    return agb_ppu_framebuffer()[y * agb_ppu_width() + x];
}

static void fill_map(int screen_base, uint16_t entry)
{
    for (int i = 0; i < 32 * 32; i++)
        *(volatile uint16_t *)(agb_mem.vram + screen_base * SCREEN_BLOCK + i * 2) = entry;
}

// Two full-screen text backgrounds: BG0 in front at priority 0, BG1 behind it at
// priority 1, each a flat colour. Both draw from tile 0; the palette bank in the
// map entry is what separates them.
static void reset_two_backgrounds(const char *name)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));

    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_BG1);
    io16w(BG0CNT, BGCNT_SCREEN_BASE(1) | 0);
    io16w(BG1CNT, BGCNT_SCREEN_BASE(2) | 1);
    memset(agb_mem.vram, 0x11, 32); // tile 0: every pixel colour index 1
    fill_map(1, 0x0000);            // BG0 uses palette bank 0 -> colour 1
    fill_map(2, 0x1000);            // BG1 uses palette bank 1 -> colour 17
    bg_pal(1, TOP);
    bg_pal(17, UNDER);
}

// An 8x8 object at (100,50), opaque, colour index 5.
static void add_object(uint16_t extra_attr0, uint16_t colour)
{
    volatile uint16_t *entry = (volatile uint16_t *)agb_mem.oam;

    memset(agb_mem.vram + OBJ_VRAM + 32, 0x55, 32);
    *(volatile uint16_t *)(agb_mem.pltt + OBJ_PLTT + 5 * 2) = colour;
    entry[0] = (uint16_t)(50 | extra_attr0);
    entry[1] = 100;
    entry[2] = 1;
}

// (16*8 + 0*8) >> 4 = 8 red, (0*8 + 8*8) >> 4 = 4 green.
static void test_alpha(void)
{
    reset_two_backgrounds("alpha blend");
    io16w(BLDCNT, TARGET_BG0 | EFFECT_ALPHA | SECOND(TARGET_BG1));
    io16w(BLDALPHA, 8 | (8 << 8));
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(BGR(8, 4, 0)), "half-and-half blend wrong: got %06X", px(0, 0));
}

// Both halves of the pairing have to be selected, or nothing happens.
static void test_alpha_needs_both_targets(void)
{
    reset_two_backgrounds("alpha with no second target");
    io16w(BLDCNT, TARGET_BG0 | EFFECT_ALPHA); // no second target at all
    io16w(BLDALPHA, 8 | (8 << 8));
    agb_ppu_render_frame();
    CHECK(px(0, 0) == argb(TOP), "blended without a second target");

    reset_two_backgrounds("alpha with the wrong first target");
    io16w(BLDCNT, TARGET_BG1 | EFFECT_ALPHA | SECOND(TARGET_BG1));
    io16w(BLDALPHA, 8 | (8 << 8));
    agb_ppu_render_frame();
    // BG1 is the layer underneath, not the top one, so it is not the first target.
    CHECK(px(0, 0) == argb(TOP), "blended with a first target that is not on top");
}

// Brighten moves each channel a fraction of the way to 31, darken a fraction of
// the way to 0.
static void test_brightness(void)
{
    reset_two_backgrounds("brightness increase");
    io16w(BLDCNT, TARGET_BG0 | EFFECT_BRIGHTEN);
    io16w(BLDY, 8);
    agb_ppu_render_frame();
    // r: 16 + ((31-16)*8 >> 4) = 23. g and b: 0 + ((31-0)*8 >> 4) = 15.
    CHECK(px(0, 0) == argb(BGR(23, 15, 15)), "brighten wrong: got %06X", px(0, 0));

    reset_two_backgrounds("brightness decrease");
    io16w(BLDCNT, TARGET_BG0 | EFFECT_DARKEN);
    io16w(BLDY, 8);
    agb_ppu_render_frame();
    // r: 16 - (16*8 >> 4) = 8. g and b were already 0.
    CHECK(px(0, 0) == argb(BGR(8, 0, 0)), "darken wrong: got %06X", px(0, 0));

    reset_two_backgrounds("brightness needs its first target");
    io16w(BLDCNT, TARGET_BG1 | EFFECT_DARKEN);
    io16w(BLDY, 8);
    agb_ppu_render_frame();
    CHECK(px(0, 0) == argb(TOP), "darkened a layer that is not the first target");
}

// The coefficients are five bits but only run to 16, so anything above saturates.
static void test_coefficients_saturate(void)
{
    reset_two_backgrounds("coefficient above 16");
    io16w(BLDCNT, TARGET_BG0 | EFFECT_ALPHA | SECOND(TARGET_BG1));
    io16w(BLDALPHA, 31 | (0 << 8)); // 31 means 16: all top, none of the layer under
    agb_ppu_render_frame();
    CHECK(px(0, 0) == argb(TOP), "a coefficient above 16 was not saturated");

    reset_two_backgrounds("brightness coefficient above 16");
    io16w(BLDCNT, TARGET_BG0 | EFFECT_BRIGHTEN);
    io16w(BLDY, 31); // means 16, so every channel goes all the way to white
    agb_ppu_render_frame();
    CHECK(px(0, 0) == argb(BGR(31, 31, 31)), "a brightness coefficient above 16 was not saturated");
}

// A sum over 31 saturates rather than wrapping into the next channel.
static void test_channel_saturates(void)
{
    reset_two_backgrounds("channel saturation");
    bg_pal(1, BGR(31, 0, 0));
    bg_pal(17, BGR(31, 0, 0));
    io16w(BLDCNT, TARGET_BG0 | EFFECT_ALPHA | SECOND(TARGET_BG1));
    io16w(BLDALPHA, 16 | (16 << 8)); // 31 + 31 = 62 before clamping
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(BGR(31, 0, 0)), "an over-bright channel did not saturate");
}

// The backdrop is a blend target like any other layer.
static void test_backdrop_target(void)
{
    reset_two_backgrounds("backdrop as second target");
    io16w(DISPCNT, DISPCNT_BG0); // BG1 off, so the backdrop is underneath
    bg_pal(0, BGR(0, 0, 8));
    io16w(BLDCNT, TARGET_BG0 | EFFECT_ALPHA | SECOND(TARGET_BACKDROP));
    io16w(BLDALPHA, 8 | (8 << 8));
    agb_ppu_render_frame();

    // r: (16*8) >> 4 = 8. b: (8*8) >> 4 = 4.
    CHECK(px(0, 0) == argb(BGR(8, 0, 4)), "the backdrop did not act as a second target");

    reset_two_backgrounds("nothing sits under the backdrop");
    io16w(DISPCNT, 0); // no layer at all, so the backdrop is on top
    bg_pal(0, BGR(16, 0, 0));
    io16w(BLDCNT, TARGET_BACKDROP | EFFECT_ALPHA | SECOND(TARGET_BACKDROP));
    // Full strength on both halves, so blending the backdrop with itself would
    // saturate to 31 rather than quietly reproducing the original colour.
    io16w(BLDALPHA, 16 | (16 << 8));
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(BGR(16, 0, 0)), "the backdrop blended with itself");
}

// A semi-transparent object blends with whatever is under it whatever the
// effect register selects -- including when it selects nothing.
static void test_semi_transparent_object(void)
{
    reset_two_backgrounds("semi-transparent object");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_OBJ | DISPCNT_1D);
    add_object(ATTR0_SEMI, TOP);
    bg_pal(1, UNDER); // the background under the object
    io16w(BLDCNT, SECOND(TARGET_BG0)); // effect "none", but BG0 is a second target
    io16w(BLDALPHA, 8 | (8 << 8));
    agb_ppu_render_frame();

    CHECK(px(102, 52) == argb(BGR(8, 4, 0)), "a semi-transparent object did not blend");
    CHECK(px(0, 0) == argb(UNDER), "the background away from the object was altered");

    reset_two_backgrounds("semi-transparent with no target under it");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_OBJ | DISPCNT_1D);
    add_object(ATTR0_SEMI, TOP);
    bg_pal(1, UNDER);
    io16w(BLDCNT, 0); // nothing is a second target
    io16w(BLDALPHA, 8 | (8 << 8));
    agb_ppu_render_frame();

    CHECK(px(102, 52) == argb(TOP), "blended a semi-transparent object with no valid target");
}

// Every effect is gated by the colour-effect bit of the window covering the
// pixel, which is what that sixth bit in each control byte is for.
static void test_window_gates_effects(void)
{
    reset_two_backgrounds("window withholds the effect");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_BG1 | DISPCNT_WIN0);
    io16w(WIN0H, (uint16_t)((40 << 8) | 100));
    io16w(WIN0V, (uint16_t)((0 << 8) | 160));
    io16w(WININ, WIN_BG0 | WIN_BG1);               // layers, but no effect bit
    io16w(WINOUT, WIN_BG0 | WIN_BG1 | WIN_EFFECT); // outside gets the effect
    io16w(BLDCNT, TARGET_BG0 | EFFECT_ALPHA | SECOND(TARGET_BG1));
    io16w(BLDALPHA, 8 | (8 << 8));
    agb_ppu_render_frame();

    CHECK(px(50, 30) == argb(TOP), "blended inside a window that withholds the effect");
    CHECK(px(150, 30) == argb(BGR(8, 4, 0)), "did not blend outside, where the effect is allowed");
}

// With no effect selected the pixel ships exactly as the layer produced it.
static void test_no_effect(void)
{
    reset_two_backgrounds("no effect selected");
    io16w(BLDCNT, TARGET_BG0 | SECOND(TARGET_BG1)); // targets set, effect 0
    io16w(BLDALPHA, 8 | (8 << 8));
    io16w(BLDY, 16);
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(TOP), "an effect was applied with none selected");
}

int main(void)
{
    test_alpha();
    test_alpha_needs_both_targets();
    test_brightness();
    test_coefficients_saturate();
    test_channel_saturates();
    test_backdrop_target();
    test_semi_transparent_object();
    test_window_gates_effects();
    test_no_effect();

    return test_report("ppu blending");
}
