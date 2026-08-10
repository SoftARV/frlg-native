// Affine background rendering, driven through the register file and VRAM.
//
// As in test_ppu_objects.c, the hardware layout is written out here rather than
// shared with the renderer: a test built on the same constant as the code under
// test cannot catch that constant being wrong.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#include "harness.h"

#define DISPCNT 0x000
#define BG0CNT 0x008
#define BG2CNT 0x00C
#define BG3CNT 0x00E
#define BG2PA 0x020
#define BG3PA 0x030

#define DISPCNT_BG(n) (0x0100 << (n))
#define DISPCNT_BG2 0x0400
#define DISPCNT_BG3 0x0800

#define BGCNT_CHAR_BASE(n) ((n) << 2)
#define BGCNT_SCREEN_BASE(n) ((n) << 8)
#define BGCNT_WRAP 0x2000
#define BGCNT_SIZE(n) ((n) << 14)

#define CHAR_BLOCK 0x4000
#define SCREEN_BLOCK 0x800

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

static void io32w(int offset, uint32_t value)
{
    *(volatile uint32_t *)(agb_mem.io + offset) = value;
}

static void bg_pal(int index, uint16_t colour)
{
    *(volatile uint16_t *)(agb_mem.pltt + index * 2) = colour;
}

// Affine matrix plus reference point, for the block belonging to BG2 or BG3.
static void affine(int block, int pa, int pb, int pc, int pd, int32_t rx, int32_t ry)
{
    io16w(block, (uint16_t)pa);
    io16w(block + 2, (uint16_t)pb);
    io16w(block + 4, (uint16_t)pc);
    io16w(block + 6, (uint16_t)pd);
    io32w(block + 8, (uint32_t)rx & 0x0FFFFFFF);
    io32w(block + 12, (uint32_t)ry & 0x0FFFFFFF);
}

// An affine map entry is a single byte: just the tile number.
static void map_entry(int screen_base, int tiles, int tx, int ty, uint8_t tile)
{
    agb_mem.vram[screen_base * SCREEN_BLOCK + ty * tiles + tx] = tile;
}

// Affine backgrounds are always 8bpp.
static void tile_px(int char_base, int tile, int x, int y, uint8_t value)
{
    agb_mem.vram[char_base * CHAR_BLOCK + tile * 64 + y * 8 + x] = value;
}

static uint32_t px(int x, int y)
{
    return agb_ppu_framebuffer()[y * agb_ppu_width() + x];
}

// Mode 1 puts BG2 on the affine path; a 16x16-tile map with an identity matrix
// should land tile (0,0) texel (0,0) exactly at the top-left of the screen.
static void setup_identity_bg2(int size, int extra_control)
{
    io16w(DISPCNT, 1 | DISPCNT_BG2);
    io16w(BG2CNT, BGCNT_CHAR_BASE(0) | BGCNT_SCREEN_BASE(1) | BGCNT_SIZE(size) | extra_control);
    affine(BG2PA, ONE, 0, 0, ONE, 0, 0);
}

static void test_identity(void)
{
    reset("affine bg identity");
    setup_identity_bg2(0, 0);
    bg_pal(7, RED);
    map_entry(1, 16, 0, 0, 5);
    tile_px(0, 5, 3, 2, 7);
    agb_ppu_render_frame();

    CHECK(px(3, 2) == argb(RED), "texel (3,2) did not land at (3,2), got %06X", px(3, 2));
    CHECK(px(2, 2) == argb(0), "colour index 0 was not transparent");
}

// One byte per entry: the tile at map (1,0) must come from the byte right after
// map (0,0), not two bytes along as a text map would.
static void test_map_is_one_byte_per_tile(void)
{
    reset("affine map entry stride");
    setup_identity_bg2(0, 0);
    bg_pal(7, GREEN);
    map_entry(1, 16, 1, 0, 5); // second tile across
    tile_px(0, 5, 0, 0, 7);
    agb_ppu_render_frame();

    CHECK(px(8, 0) == argb(GREEN), "second map column did not land at x=8");
    CHECK(px(0, 0) == argb(0), "the first map column drew something");
}

// The reference point is 20.8 fixed point, so 8 whole pixels is 8 << 8.
static void test_reference_point(void)
{
    reset("affine bg reference point");
    io16w(DISPCNT, 1 | DISPCNT_BG2);
    io16w(BG2CNT, BGCNT_CHAR_BASE(0) | BGCNT_SCREEN_BASE(1));
    affine(BG2PA, ONE, 0, 0, ONE, 8 << 8, 0);
    bg_pal(7, BLUE);
    map_entry(1, 16, 1, 0, 5);
    tile_px(0, 5, 0, 0, 7);
    agb_ppu_render_frame();

    // Scrolling the map 8 pixels right pulls the second tile to the origin.
    CHECK(px(0, 0) == argb(BLUE), "reference point did not shift the map");

    reset("affine bg negative reference point");
    io16w(DISPCNT, 1 | DISPCNT_BG2);
    io16w(BG2CNT, BGCNT_CHAR_BASE(0) | BGCNT_SCREEN_BASE(1));
    affine(BG2PA, ONE, 0, 0, ONE, -(8 << 8), 0);
    bg_pal(7, BLUE);
    map_entry(1, 16, 0, 0, 5);
    tile_px(0, 5, 0, 0, 7);
    agb_ppu_render_frame();

    // A negative reference must sign-extend out of its 28 bits, not wrap huge.
    CHECK(px(8, 0) == argb(BLUE), "negative reference point was not sign-extended");
}

// pb and pd move the sample point down the screen, one step per scanline.
static void test_per_scanline_step(void)
{
    reset("affine bg vertical step");
    setup_identity_bg2(0, 0);
    bg_pal(7, WHITE);
    map_entry(1, 16, 0, 0, 5);
    tile_px(0, 5, 0, 4, 7); // texel row 4
    agb_ppu_render_frame();

    CHECK(px(0, 4) == argb(WHITE), "pd did not advance the sample point per scanline");
    CHECK(px(0, 3) == argb(0), "drew on the wrong scanline");
}

static void test_scale_and_rotation(void)
{
    reset("affine bg scale");
    io16w(DISPCNT, 1 | DISPCNT_BG2);
    io16w(BG2CNT, BGCNT_CHAR_BASE(0) | BGCNT_SCREEN_BASE(1));
    affine(BG2PA, 2 * ONE, 0, 0, 2 * ONE, 0, 0); // two map pixels per screen pixel
    bg_pal(7, RED);
    map_entry(1, 16, 0, 0, 5);
    tile_px(0, 5, 4, 0, 7);
    agb_ppu_render_frame();

    CHECK(px(2, 0) == argb(RED), "map texel 4 did not halve to screen x=2");

    reset("affine bg rotation");
    io16w(DISPCNT, 1 | DISPCNT_BG2);
    io16w(BG2CNT, BGCNT_CHAR_BASE(0) | BGCNT_SCREEN_BASE(1));
    // Quarter turn: screen x walks down the map, screen y walks across it.
    affine(BG2PA, 0, ONE, ONE, 0, 0, 0);
    bg_pal(7, GREEN);
    map_entry(1, 16, 0, 0, 5);
    tile_px(0, 5, 0, 3, 7); // map (0,3)
    agb_ppu_render_frame();

    CHECK(px(3, 0) == argb(GREEN), "rotated sample did not land at (3,0)");
    CHECK(px(0, 3) == argb(0), "sample landed at its unrotated position");
}

// Off the edge of the map is transparent, unless the control bit says wrap.
static void test_overflow(void)
{
    reset("affine bg overflow is transparent");
    io16w(DISPCNT, 1 | DISPCNT_BG2);
    io16w(BG2CNT, BGCNT_CHAR_BASE(0) | BGCNT_SCREEN_BASE(1));
    // Start one whole map to the left, so the screen is entirely off-map.
    affine(BG2PA, ONE, 0, 0, ONE, -(200 << 8), 0);
    bg_pal(7, RED);
    memset(agb_mem.vram + SCREEN_BLOCK, 5, 16 * 16); // every entry is tile 5
    memset(agb_mem.vram + 5 * 64, 7, 64);            // every texel opaque
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(0), "sampled outside a non-wrapping map");
    CHECK(px(50, 0) == argb(0), "sampled outside a non-wrapping map");

    reset("affine bg overflow wraps");
    io16w(DISPCNT, 1 | DISPCNT_BG2);
    io16w(BG2CNT, BGCNT_CHAR_BASE(0) | BGCNT_SCREEN_BASE(1) | BGCNT_WRAP);
    affine(BG2PA, ONE, 0, 0, ONE, -(200 << 8), 0);
    bg_pal(7, RED);
    memset(agb_mem.vram + SCREEN_BLOCK, 5, 16 * 16);
    memset(agb_mem.vram + 5 * 64, 7, 64);
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(RED), "wrap bit did not bring the map back around");
}

// Size selects a square map of 16, 32, 64 or 128 tiles a side, and the map
// stride has to follow it.
static void test_map_sizes(void)
{
    const int tiles[4] = {16, 32, 64, 128};

    for (int size = 0; size < 4; size++)
    {
        reset("affine bg map size");
        setup_identity_bg2(size, 0);
        bg_pal(7, WHITE);
        map_entry(1, tiles[size], 0, 1, 5); // one tile row down
        tile_px(0, 5, 0, 0, 7);
        agb_ppu_render_frame();

        CHECK(px(0, 8) == argb(WHITE), "size %d: second map row misplaced", size);
    }
}

// Mode 2 puts both BG2 and BG3 on the affine path. Before this existed they
// fell through to the text renderer, which drew a plausible wrong picture.
static void test_mode2_uses_the_affine_path(void)
{
    reset("mode 2 renders bg3 as affine");
    io16w(DISPCNT, 2 | DISPCNT_BG3);
    io16w(BG3CNT, BGCNT_CHAR_BASE(0) | BGCNT_SCREEN_BASE(1));
    affine(BG3PA, ONE, 0, 0, ONE, 0, 0);
    bg_pal(7, GREEN);
    map_entry(1, 16, 1, 0, 5);
    tile_px(0, 5, 0, 0, 7);
    agb_ppu_render_frame();

    // A text renderer would read this map two bytes per entry and land elsewhere.
    CHECK(px(8, 0) == argb(GREEN), "bg3 in mode 2 did not take the affine path");

    reset("mode 1 does not draw bg3");
    io16w(DISPCNT, 1 | DISPCNT_BG3);
    io16w(BG3CNT, BGCNT_CHAR_BASE(1) | BGCNT_SCREEN_BASE(1));
    affine(BG3PA, ONE, 0, 0, ONE, 0, 0);
    // Whichever path a mistake sent this down has to produce a visible pixel,
    // or "nothing drawn" proves nothing: the whole character block is opaque,
    // and both the affine and the text reading of it are given a palette entry.
    memset(agb_mem.vram + SCREEN_BLOCK, 5, 16 * 16);
    memset(agb_mem.vram + CHAR_BLOCK, 0x77, CHAR_BLOCK);
    bg_pal(7, GREEN);     // what the text path would resolve to
    bg_pal(0x77, GREEN);  // what the affine path would resolve to
    agb_ppu_render_frame();

    CHECK(px(0, 0) == argb(0), "mode 1 drew bg3, which that mode does not define");
}

// Which layers each mode defines. A layer outside its mode's set is not drawn
// at all, so this pins the whole table rather than the two rows the affine work
// happened to touch. The character block is opaque and both the text and affine
// readings of it have a palette entry, so a layer drawn by mistake shows up
// whichever path the mistake sent it down.
static void test_mode_layer_matrix(void)
{
    static const struct
    {
        int mode;
        int bg;
        int drawn;
    } cases[] = {
        {0, 0, 1}, {0, 1, 1}, {0, 2, 1}, {0, 3, 1}, // four text layers
        {1, 0, 1}, {1, 1, 1}, {1, 2, 1}, {1, 3, 0}, // two text, BG2 affine
        {2, 0, 0}, {2, 1, 0}, {2, 2, 1}, {2, 3, 1}, // BG2 and BG3 affine
        {6, 2, 0}, {7, 2, 0},                       // prohibited, no layer at all
    };
    // Modes 3 to 5 put a frame buffer on BG2 and are covered by
    // test_ppu_bitmap.c, which can read one back.


    for (int i = 0; i < (int)(sizeof cases / sizeof cases[0]); i++)
    {
        int mode = cases[i].mode;
        int bg = cases[i].bg;
        int drawn;

        reset("mode/layer matrix");
        io16w(DISPCNT, (uint16_t)(mode | DISPCNT_BG(bg)));
        io16w(BG0CNT + bg * 2, BGCNT_CHAR_BASE(1) | BGCNT_SCREEN_BASE(1));
        affine(BG2PA, ONE, 0, 0, ONE, 0, 0);
        affine(BG3PA, ONE, 0, 0, ONE, 0, 0);
        memset(agb_mem.vram + SCREEN_BLOCK, 5, 16 * 16);
        memset(agb_mem.vram + CHAR_BLOCK, 0x77, CHAR_BLOCK);
        bg_pal(7, GREEN);    // what a text reading resolves to
        bg_pal(0x77, GREEN); // what an affine reading resolves to
        agb_ppu_render_frame();

        drawn = px(0, 0) == argb(GREEN);
        CHECK(drawn == cases[i].drawn, "mode %d bg %d was %s", mode, bg,
              drawn ? "drawn but should not be" : "not drawn but should be");
    }
}

int main(void)
{
    test_identity();
    test_map_is_one_byte_per_tile();
    test_reference_point();
    test_per_scanline_step();
    test_scale_and_rotation();
    test_overflow();
    test_map_sizes();
    test_mode2_uses_the_affine_path();
    test_mode_layer_matrix();

    return test_report("ppu affine backgrounds");
}
