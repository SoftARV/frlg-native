// Window masking: the two rectangular windows, the object window, and the
// region outside all of them.
//
// As elsewhere in tests/, the register layout is written out here rather than
// shared with the renderer, so a wrong constant cannot cancel itself out.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define OBJ_VRAM 0x10000
#define OBJ_PLTT 0x200
#define SCREEN_BLOCK 0x800

#define DISPCNT 0x000
#define BG0CNT 0x008
#define BG2CNT 0x00C
#define BG2PA 0x020
#define WIN0H 0x040
#define WIN1H 0x042
#define WIN0V 0x044
#define WIN1V 0x046
#define WININ 0x048
#define WINOUT 0x04A

#define DISPCNT_BG0 0x0100
#define BG2_ON 0x0400
#define DISPCNT_OBJ 0x1000
#define DISPCNT_1D 0x0040
#define DISPCNT_WIN0 0x2000
#define DISPCNT_WIN1 0x4000
#define DISPCNT_WIN_OBJ 0x8000

#define BGCNT_SCREEN_BASE(n) ((n) << 8)

#define WIN_BG0 0x01
#define WIN_BG2 0x04
#define WIN_OBJ 0x10

#define ATTR0_OBJ_WINDOW (2 << 10)

#define RED 0x001F
#define BLUE 0x7C00

// A window register holds its start in the high byte and its end in the low one.
#define BOUNDS(start, end) (uint16_t)(((start) << 8) | (end))

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

static uint32_t px(int x, int y)
{
    return agb_ppu_framebuffer()[y * agb_ppu_width() + x];
}

// A text BG0 covering the whole screen in one flat colour, so a masked pixel is
// unambiguous: either the background is there or the backdrop is.
static void reset_with_bg0(const char *name)
{
    TEST_CASE(name);
    memset(&agb_mem, 0, sizeof(agb_mem));

    io16w(BG0CNT, BGCNT_SCREEN_BASE(1)); // map at 0x800, tiles at 0
    memset(agb_mem.vram, 0x11, 32);      // tile 0: every pixel colour index 1
    *(volatile uint16_t *)(agb_mem.pltt + 2) = BLUE;
}

// An 8x8 object at (100,50), opaque throughout.
static void add_object(uint16_t extra_attr0)
{
    volatile uint16_t *entry = (volatile uint16_t *)agb_mem.oam;

    memset(agb_mem.vram + OBJ_VRAM + 32, 0x55, 32); // tile 1, colour index 5
    *(volatile uint16_t *)(agb_mem.pltt + OBJ_PLTT + 5 * 2) = RED;
    entry[0] = (uint16_t)(50 | extra_attr0);
    entry[1] = 100;
    entry[2] = 1;
}

static void test_window0_rectangle(void)
{
    reset_with_bg0("window 0 bounds");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_WIN0);
    io16w(WIN0H, BOUNDS(40, 100));
    io16w(WIN0V, BOUNDS(20, 60));
    io16w(WININ, WIN_BG0); // background visible inside
    io16w(WINOUT, 0);      // and nowhere else
    agb_ppu_render_frame();

    CHECK(px(50, 30) == argb(BLUE), "the background was not drawn inside the window");
    CHECK(px(10, 30) == argb(0), "the background was drawn left of the window");
    CHECK(px(150, 30) == argb(0), "the background was drawn right of the window");
    CHECK(px(50, 10) == argb(0), "the background was drawn above the window");
    CHECK(px(50, 100) == argb(0), "the background was drawn below the window");

    // The start is inclusive and the end exclusive, on both axes.
    CHECK(px(39, 30) == argb(0), "left edge was inclusive one pixel early");
    CHECK(px(40, 30) == argb(BLUE), "left edge excluded its own column");
    CHECK(px(99, 30) == argb(BLUE), "right edge excluded a column inside");
    CHECK(px(100, 30) == argb(0), "right edge was inclusive");
    CHECK(px(50, 19) == argb(0), "top edge was inclusive one row early");
    CHECK(px(50, 20) == argb(BLUE), "top edge excluded its own row");
    CHECK(px(50, 59) == argb(BLUE), "bottom edge excluded a row inside");
    CHECK(px(50, 60) == argb(0), "bottom edge was inclusive");
}

// Inside and outside are two independent control bytes, not one and its inverse.
static void test_outside_is_its_own_region(void)
{
    reset_with_bg0("outside window control");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_WIN0);
    io16w(WIN0H, BOUNDS(40, 100));
    io16w(WIN0V, BOUNDS(20, 60));
    io16w(WININ, 0);       // hidden inside
    io16w(WINOUT, WIN_BG0); // shown outside
    agb_ppu_render_frame();

    CHECK(px(50, 30) == argb(0), "the background was drawn inside a window that hides it");
    CHECK(px(10, 30) == argb(BLUE), "the background was not drawn outside");
}

// Window 0 outranks window 1 wherever they overlap.
static void test_window_precedence(void)
{
    reset_with_bg0("window 0 outranks window 1");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_WIN0 | DISPCNT_WIN1);
    io16w(WIN0H, BOUNDS(40, 100));
    io16w(WIN0V, BOUNDS(0, 160));
    io16w(WIN1H, BOUNDS(80, 140));
    io16w(WIN1V, BOUNDS(0, 160));
    io16w(WININ, WIN_BG0); // visible in window 0, hidden in window 1
    io16w(WINOUT, 0);
    agb_ppu_render_frame();

    CHECK(px(50, 30) == argb(BLUE), "window 0 alone did not show the background");
    CHECK(px(90, 30) == argb(BLUE), "the overlap did not belong to window 0");
    CHECK(px(120, 30) == argb(0), "window 1 alone did not hide the background");
    CHECK(px(200, 30) == argb(0), "outside did not hide the background");
}

// An object in the window graphics mode contributes its shape as a region, and
// is never drawn as colour.
static void test_object_window(void)
{
    reset_with_bg0("object window region");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_OBJ | DISPCNT_1D | DISPCNT_WIN_OBJ);
    add_object(ATTR0_OBJ_WINDOW);
    io16w(WININ, 0);
    io16w(WINOUT, (WIN_BG0 << 8)); // background visible only in the object window
    agb_ppu_render_frame();

    CHECK(px(102, 52) == argb(BLUE), "the object window did not become a region");
    CHECK(px(102, 52) != argb(RED), "the window object was drawn as colour");
    CHECK(px(50, 52) == argb(0), "the background was drawn outside the object window");
    CHECK(px(108, 52) == argb(0), "the region extended past the object");
}

// The object layer has its own bit in every window control byte.
static void test_object_layer_is_masked(void)
{
    reset_with_bg0("object layer masking");
    io16w(DISPCNT, DISPCNT_OBJ | DISPCNT_1D | DISPCNT_WIN0); // no background
    add_object(0);
    io16w(WIN0H, BOUNDS(100, 104));
    io16w(WIN0V, BOUNDS(0, 160));
    io16w(WININ, WIN_OBJ);
    io16w(WINOUT, 0);
    agb_ppu_render_frame();

    CHECK(px(102, 52) == argb(RED), "the object was hidden inside a window that shows it");
    CHECK(px(105, 52) == argb(0), "the object was drawn outside the window");
}

// An end past the screen, or before the start, is garbage that reads as the
// far edge rather than as an empty or wrapping window.
static void test_garbage_bounds(void)
{
    reset_with_bg0("horizontal end past the screen");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_WIN0);
    io16w(WIN0H, BOUNDS(40, 250));
    io16w(WIN0V, BOUNDS(0, 160));
    io16w(WININ, WIN_BG0);
    io16w(WINOUT, 0);
    agb_ppu_render_frame();
    CHECK(px(239, 30) == argb(BLUE), "an end past the screen did not reach the edge");
    CHECK(px(10, 30) == argb(0), "the start was ignored as well");

    reset_with_bg0("horizontal start after end");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_WIN0);
    io16w(WIN0H, BOUNDS(100, 40));
    io16w(WIN0V, BOUNDS(0, 160));
    io16w(WININ, WIN_BG0);
    io16w(WINOUT, 0);
    agb_ppu_render_frame();
    CHECK(px(150, 30) == argb(BLUE), "a start after the end did not run to the edge");
    CHECK(px(50, 30) == argb(0), "a start after the end drew before the start");

    reset_with_bg0("vertical end past the screen");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_WIN0);
    io16w(WIN0H, BOUNDS(0, 240));
    io16w(WIN0V, BOUNDS(20, 200));
    io16w(WININ, WIN_BG0);
    io16w(WINOUT, 0);
    agb_ppu_render_frame();
    CHECK(px(50, 159) == argb(BLUE), "a vertical end past the screen did not reach the edge");
    CHECK(px(50, 10) == argb(0), "the vertical start was ignored as well");
}

// With no window enabled there is no masking, whatever the control registers
// happen to hold -- which is every frame drawn before windows existed.
static void test_no_window_enabled(void)
{
    reset_with_bg0("no window enabled");
    io16w(DISPCNT, DISPCNT_BG0); // no window bits
    io16w(WIN0H, BOUNDS(40, 100));
    io16w(WIN0V, BOUNDS(20, 60));
    io16w(WININ, 0);  // would hide everything
    io16w(WINOUT, 0); // if it were consulted
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(BLUE), "masking was applied with no window enabled");
    CHECK(px(120, 80) == argb(BLUE), "masking was applied with no window enabled");
    CHECK(px(239, 159) == argb(BLUE), "masking was applied with no window enabled");
}

// Enabling a window in DISPCNT is what turns masking on; the object window is
// its own enable, separate from the two rectangles.
static void test_enable_bits(void)
{
    reset_with_bg0("object window needs its enable bit");
    io16w(DISPCNT, DISPCNT_BG0 | DISPCNT_OBJ | DISPCNT_1D | DISPCNT_WIN0);
    add_object(ATTR0_OBJ_WINDOW);
    io16w(WIN0H, BOUNDS(0, 0)); // an empty window, so everything is "outside"
    io16w(WIN0V, BOUNDS(0, 0));
    io16w(WININ, 0);
    io16w(WINOUT, (WIN_BG0 << 8)); // only the object window would show anything
    agb_ppu_render_frame();

    CHECK(px(102, 52) == argb(0), "the object window applied without its enable bit");
}

// The affine renderer carries its own copy of the mask test, so masking a text
// background proves nothing about it.
static void test_affine_background_is_masked(void)
{
    TEST_CASE("affine background masking");
    memset(&agb_mem, 0, sizeof(agb_mem));

    io16w(DISPCNT, 1 | BG2_ON | DISPCNT_WIN0); // mode 1 puts BG2 on the affine path
    io16w(BG2CNT, BGCNT_SCREEN_BASE(1));
    io16w(BG2PA, 0x100);
    io16w(BG2PA + 6, 0x100);
    io32w(BG2PA + 8, 0);
    io32w(BG2PA + 12, 0);
    memset(agb_mem.vram + SCREEN_BLOCK, 5, 16 * 16); // every entry is tile 5
    memset(agb_mem.vram + 5 * 64, 0x77, 64);         // and every texel opaque
    *(volatile uint16_t *)(agb_mem.pltt + 0x77 * 2) = BLUE;
    io16w(WIN0H, BOUNDS(40, 100));
    io16w(WIN0V, BOUNDS(20, 60));
    io16w(WININ, WIN_BG2);
    io16w(WINOUT, 0);
    agb_ppu_render_frame();

    CHECK(px(50, 30) == argb(BLUE), "the affine background was hidden inside its window");
    CHECK(px(10, 30) == argb(0), "the affine background was drawn outside its window");
    CHECK(px(50, 100) == argb(0), "the affine background was drawn below its window");
}

int main(void)
{
    test_window0_rectangle();
    test_affine_background_is_masked();
    test_outside_is_its_own_region();
    test_window_precedence();
    test_object_window();
    test_object_layer_is_masked();
    test_garbage_bounds();
    test_no_window_enabled();
    test_enable_bits();

    return test_report("ppu windows");
}
