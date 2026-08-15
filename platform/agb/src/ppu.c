// Scanline renderer.
//
// Every display mode and every layer feature the hardware has: backgrounds,
// text, affine and bitmap; objects, regular and affine; the windows that mask
// them; the colour effects; and mosaic.
//
// Composition walks the screen a scanline at a time and re-reads every register
// as it goes, so a per-scanline effect writing from an H-blank handler lands on
// the following line -- which is what the game's battle transitions are built
// out of.

#include <string.h>

#include "agb/dma.h"
#include "agb/irq.h"
#include "agb/memmap.h"
#include "agb/ppu.h"

// What the hardware itself is: the size of a bitmap-mode frame in VRAM, and the
// floor for any viewport. The GBA is 240x160 and that does not change.
#define NATIVE_W 240
#define NATIVE_H 160

// What this renderer will compose. The viewport is a window onto the same
// backgrounds the game already draws; nothing here decides how much of the
// world exists, only how much of it is asked for. Buffers are sized for the
// largest viewport rather than reallocated, so a resize costs nothing and
// cannot fail halfway.
#define SCREEN_MAX_W 512
#define SCREEN_MAX_H 512

static int screen_w = NATIVE_W;
static int screen_h = NATIVE_H;

// A viewport wider than the hardware is centred on it, so the extra comes from
// every side rather than accumulating at the right and the bottom. Everything
// the game positions in the hardware's own coordinates -- scroll offsets, object
// positions, window edges -- is shifted by this to land where it always did.
static int view_ox(void)
{
    return (screen_w - NATIVE_W) / 2;
}

static int view_oy(void)
{
    return (screen_h - NATIVE_H) / 2;
}

#define REG_DISPCNT 0x000
#define REG_DISPSTAT 0x004
#define REG_VCOUNT 0x006
#define REG_BG0CNT 0x008
#define REG_BG0HOFS 0x010
// BG2's affine block; BG3's follows 0x10 later.
#define REG_BG2PA 0x020
#define REG_AFFINE_BLOCK(bg) (REG_BG2PA + ((bg) - 2) * 0x10)

#define REG_WIN0H 0x040
#define REG_WIN0V 0x044
#define REG_WININ 0x048
#define REG_WINOUT 0x04A
#define REG_MOSAIC 0x04C
#define REG_BLDCNT 0x050
#define REG_BLDALPHA 0x052
#define REG_BLDY 0x054

// Each mosaic field holds one less than the size of a block, so zero means a
// block of one pixel -- which is no mosaic at all.
#define MOSAIC_BG_H(m) (((m) & 0xF) + 1)
#define MOSAIC_BG_V(m) ((((m) >> 4) & 0xF) + 1)
#define MOSAIC_OBJ_H(m) ((((m) >> 8) & 0xF) + 1)
#define MOSAIC_OBJ_V(m) ((((m) >> 12) & 0xF) + 1)

#define BLD_EFFECT(c) (((c) >> 6) & 3)
#define BLD_ALPHA 1
#define BLD_BRIGHTEN 2
#define BLD_DARKEN 3
#define BLD_FIRST_TARGET(c) ((c) & 0x3F)
#define BLD_SECOND_TARGET(c) (((c) >> 8) & 0x3F)

// Layer identities, in the order the blend target bits name them.
#define LAYER_OBJ 4
#define LAYER_BACKDROP 5
#define LAYER_NONE 0xFF

#define BGR_R(c) ((c) & 0x1F)
#define BGR_G(c) (((c) >> 5) & 0x1F)
#define BGR_B(c) (((c) >> 10) & 0x1F)
#define BGR(r, g, b) ((uint16_t)((r) | ((g) << 5) | ((b) << 10)))

#define DISPSTAT_VBLANK_FLAG 0x0001
#define DISPSTAT_HBLANK_FLAG 0x0002
#define DISPSTAT_VCOUNT_FLAG 0x0004
#define DISPSTAT_HBLANK_IRQ 0x0010
#define DISPSTAT_VCOUNT_IRQ 0x0020
#define DISPSTAT_VCOUNT_SETTING(d) (((d) >> 8) & 0xFF)

#define DISPCNT_MODE_MASK 0x0007
#define DISPCNT_FRAME_SELECT 0x0010
#define DISPCNT_OBJ_1D_MAP 0x0040
#define DISPCNT_FORCED_BLANK 0x0080
#define DISPCNT_BG_ENABLE(n) (0x0100 << (n))
#define DISPCNT_OBJ_ENABLE 0x1000
#define DISPCNT_WIN0 0x2000
#define DISPCNT_WIN1 0x4000
#define DISPCNT_WIN_OBJ 0x8000

// A window control byte is one bit per background, then the object layer, then
// the colour effects.
#define WINDOW_OBJ 0x10
#define WINDOW_EFFECT 0x20
#define WINDOW_CONTROL_MASK 0x3F
#define WINDOW_EVERYTHING WINDOW_CONTROL_MASK

#define BGCNT_PRIORITY_MASK 0x0003
#define BGCNT_CHAR_BASE(c) (((c) >> 2) & 3)
#define BGCNT_MOSAIC 0x0040
#define BGCNT_256_COLOUR 0x0080
#define BGCNT_SCREEN_BASE(c) (((c) >> 8) & 0x1F)
// Affine only: off the edge of the map is transparent unless this says to wrap.
#define BGCNT_AFFINE_WRAP 0x2000
#define BGCNT_SIZE(c) (((c) >> 14) & 3)

// An affine map is square and sized in tiles: 16, 32, 64 or 128 a side.
#define BGCNT_AFFINE_TILES(c) (16 << BGCNT_SIZE(c))

// The bitmap modes define one layer, on BG2, and nothing else.
#define BITMAP_BG 2
#define BITMAP_FRAME_SIZE 0xA000
#define BITMAP_MODE5_W 160
#define BITMAP_MODE5_H 128

#define CHAR_BLOCK_SIZE 0x4000
#define SCREEN_BLOCK_SIZE 0x800
#define TILE_SIZE_4BPP 32
#define TILE_SIZE_8BPP 64

#define MAP_TILE_INDEX(e) ((e) & 0x03FF)
#define MAP_HFLIP 0x0400
#define MAP_VFLIP 0x0800
#define MAP_PALETTE(e) (((e) >> 12) & 0xF)

#define OBJ_COUNT 128
#define OBJ_VRAM_BASE 0x10000
#define OBJ_VRAM_SIZE 0x8000
#define OBJ_PLTT_BASE 0x200
#define OBJ_2D_ROW_STRIDE (32 * TILE_SIZE_4BPP)
// In the bitmap modes the first 512 tile slots are the frame buffer's.
#define OBJ_BITMAP_MIN_TILE 512

#define OBJ_Y(a) ((a) & 0xFF)
#define OBJ_MODE(a) (((a) >> 8) & 3)
#define OBJ_MODE_NORMAL 0
#define OBJ_MODE_HIDDEN 2
#define OBJ_MODE_AFFINE_DOUBLE 3
#define OBJ_GFX_MODE(a) (((a) >> 10) & 3)
#define OBJ_GFX_SEMI 1
#define OBJ_GFX_WINDOW 2
#define OBJ_MOSAIC 0x1000
#define OBJ_256_COLOUR 0x2000
#define OBJ_SHAPE(a) (((a) >> 14) & 3)

#define OBJ_X(a) ((a) & 0x1FF)
#define OBJ_HFLIP 0x1000
#define OBJ_VFLIP 0x2000
#define OBJ_SIZE(a) (((a) >> 14) & 3)

// An affine object replaces the two flip bits with a 5-bit selector. The matrix
// itself is interleaved into the unused fourth halfword of four OAM entries, so
// group g lives in entries 4g..4g+3 and costs those entries nothing.
#define OBJ_AFFINE_GROUP(a) (((a) >> 9) & 0x1F)
#define OBJ_AFFINE_PARAM(g, i) ((((g) * 4 + (i)) * 8) + 6)
#define OBJ_AFFINE_FRACTION 8

#define OBJ_TILE_INDEX(a) ((a) & 0x03FF)
#define OBJ_PRIORITY(a) (((a) >> 10) & 3)
#define OBJ_PALETTE(a) (((a) >> 12) & 0xF)

static const uint8_t obj_dimensions[4][4][2] = {
    {{8, 8}, {16, 16}, {32, 32}, {64, 64}},
    {{16, 8}, {32, 8}, {32, 16}, {64, 32}},
    {{8, 16}, {8, 32}, {16, 32}, {32, 64}},
    {{0, 0}, {0, 0}, {0, 0}, {0, 0}}, // prohibited; zero height draws nothing
};

static uint32_t framebuffer[SCREEN_MAX_W * SCREEN_MAX_H];
static uint32_t frame_serial;

// One scanline of the object layer, resolved before any background is drawn.
// Index 0 means no object claimed the pixel.
static uint8_t obj_colour[SCREEN_MAX_W];
static uint8_t obj_prio[SCREEN_MAX_W];

// Objects in the window graphics mode contribute shape rather than colour, so
// they land here instead, and become a region in the mask below.
static uint8_t obj_window[SCREEN_MAX_W];

// Set where the object that won the pixel asked to be blended with whatever is
// under it, whatever the blend registers otherwise select.
static uint8_t obj_semi[SCREEN_MAX_W];

// Which layers may draw at each pixel of the current scanline.
static uint8_t window_mask[SCREEN_MAX_W];

// Blending needs the layer under the top one, so composition collects the two
// frontmost contributors per pixel rather than stopping at the first. Anything
// below them cannot affect the result.
static uint16_t layer_colour[2][SCREEN_MAX_W];
static uint8_t layer_id[2][SCREEN_MAX_W];
static uint8_t layer_count[SCREEN_MAX_W];

// Layers arrive front to back, so the first two to claim a pixel are the two
// that matter and the rest are discarded.
static void deposit(int x, uint16_t colour, int id)
{
    int slot = layer_count[x];

    layer_colour[slot][x] = colour;
    layer_id[slot][x] = (uint8_t)id;
    layer_count[x] = (uint8_t)(slot + 1);
}

bool agb_ppu_set_viewport(int width, int height)
{
    int w = width < AGB_PPU_MIN_W ? AGB_PPU_MIN_W
          : width > AGB_PPU_MAX_W ? AGB_PPU_MAX_W : width;
    int h = height < AGB_PPU_MIN_H ? AGB_PPU_MIN_H
          : height > AGB_PPU_MAX_H ? AGB_PPU_MAX_H : height;

    if (w == screen_w && h == screen_h)
        return false;

    screen_w = w;
    screen_h = h;
    return true;
}

int agb_ppu_width(void)
{
    return screen_w;
}

int agb_ppu_height(void)
{
    return screen_h;
}

const uint32_t *agb_ppu_framebuffer(void)
{
    return framebuffer;
}

uint32_t agb_ppu_frame_serial(void)
{
    return frame_serial;
}

static uint16_t io16(int offset)
{
    return *(const volatile uint16_t *)(agb_mem.io + offset);
}

static void io16_write(int offset, uint16_t value)
{
    *(volatile uint16_t *)(agb_mem.io + offset) = value;
}

static uint32_t io32(int offset)
{
    return *(const volatile uint32_t *)(agb_mem.io + offset);
}

static uint16_t oam16(int offset)
{
    return *(const volatile uint16_t *)(agb_mem.oam + offset);
}

// An affine background's reference point is 28 bits of signed 20.8 fixed point
// sitting in a 32-bit register, so the unused top nibble has to be sign-extended
// away rather than masked off.
static int32_t bg_reference(uint32_t raw)
{
    int32_t value = (int32_t)(raw & 0x0FFFFFFF);

    return (value & 0x08000000) ? value - 0x10000000 : value;
}

// Mosaic keeps the pixel at the start of each block and repeats it across the
// rest, so a coordinate is snapped back to its block. A size of one is the
// identity, which is what a layer with mosaic switched off passes in.
static int mosaic_snap(int value, int size)
{
    return value - value % size;
}

static uint16_t bg_palette(int index)
{
    return *(const volatile uint16_t *)(agb_mem.pltt + index * 2);
}

static uint16_t obj_palette(int index)
{
    return *(const volatile uint16_t *)(agb_mem.pltt + OBJ_PLTT_BASE + index * 2);
}

// Tile index, mapping stride and 8bpp tile size can between them address past
// the 32 KiB object region, so the offset is wrapped inside it rather than
// allowed to walk into the neighbouring memory.
static uint8_t obj_vram(uint32_t offset)
{
    return agb_mem.vram[OBJ_VRAM_BASE + (offset & (OBJ_VRAM_SIZE - 1))];
}

// BGR555 -> XRGB8888, replicating the high bits so white reaches 0xFF.
static uint32_t to_argb(uint16_t bgr)
{
    uint32_t r = (bgr & 0x1F) << 3;
    uint32_t g = ((bgr >> 5) & 0x1F) << 3;
    uint32_t b = ((bgr >> 10) & 0x1F) << 3;

    r |= r >> 5;
    g |= g >> 5;
    b |= b >> 5;
    return (r << 16) | (g << 8) | b;
}

// Text backgrounds wrap, and sizes wider or taller than one screen block are
// stored as separate blocks rather than as one wide map.
static int screen_block_offset(int size, int map_x, int map_y)
{
    int block = 0;

    switch (size)
    {
    case 1: // 512x256
        block = (map_x >> 5) & 1;
        break;
    case 2: // 256x512
        block = (map_y >> 5) & 1;
        break;
    case 3: // 512x512
        block = (((map_y >> 5) & 1) << 1) | ((map_x >> 5) & 1);
        break;
    default:
        break;
    }

    return block * SCREEN_BLOCK_SIZE;
}

static void render_text_bg_line(int bg, int line)
{
    uint16_t control = io16(REG_BG0CNT + bg * 2);
    int hofs = io16(REG_BG0HOFS + bg * 4) & 0x1FF;
    int vofs = io16(REG_BG0HOFS + bg * 4 + 2) & 0x1FF;
    int size = BGCNT_SIZE(control);
    int is_256 = (control & BGCNT_256_COLOUR) != 0;
    const uint8_t *chars = agb_mem.vram + BGCNT_CHAR_BASE(control) * CHAR_BLOCK_SIZE;
    const uint8_t *screen = agb_mem.vram + BGCNT_SCREEN_BASE(control) * SCREEN_BLOCK_SIZE;

    uint16_t mosaic = io16(REG_MOSAIC);
    int mos_h = (control & BGCNT_MOSAIC) ? MOSAIC_BG_H(mosaic) : 1;
    int mos_v = (control & BGCNT_MOSAIC) ? MOSAIC_BG_V(mosaic) : 1;
    int width_mask = (size == 1 || size == 3) ? 0x1FF : 0xFF;
    int height_mask = (size == 2 || size == 3) ? 0x1FF : 0xFF;
    int src_y = (mosaic_snap(line, mos_v) - view_oy() + vofs) & height_mask;

    for (int x = 0; x < screen_w; x++)
    {
        int src_x = (mosaic_snap(x, mos_h) - view_ox() + hofs) & width_mask;
        int map_x = src_x >> 3;
        int map_y = src_y >> 3;
        const uint8_t *block = screen + screen_block_offset(size, map_x, map_y);
        int entry_index = ((map_y & 31) * 32 + (map_x & 31)) * 2;
        uint16_t entry = (uint16_t)(block[entry_index] | (block[entry_index + 1] << 8));

        int px = src_x & 7;
        int py = src_y & 7;
        int colour;

        if (entry & MAP_HFLIP)
            px = 7 - px;
        if (entry & MAP_VFLIP)
            py = 7 - py;

        if (is_256)
        {
            const uint8_t *tile = chars + MAP_TILE_INDEX(entry) * TILE_SIZE_8BPP;
            colour = tile[py * 8 + px];
            if (colour == 0)
                continue;
        }
        else
        {
            const uint8_t *tile = chars + MAP_TILE_INDEX(entry) * TILE_SIZE_4BPP;
            uint8_t pair = tile[py * 4 + (px >> 1)];

            colour = (px & 1) ? (pair >> 4) : (pair & 0xF);
            if (colour == 0)
                continue;
            colour += MAP_PALETTE(entry) * 16;
        }

        if (layer_count[x] >= 2 || !(window_mask[x] & (1 << bg)))
            continue;

        deposit(x, bg_palette(colour), bg);
    }
}

// One edge of a window, from a register holding its start in the high byte and
// its end in the low one. An end past the screen, or before the start, is
// garbage that hardware reads as the far edge.
static int window_contains(uint16_t bounds, int value, int limit)
{
    int start = (bounds >> 8) & 0xFF;
    int end = bounds & 0xFF;

    if (end > limit || start > end)
        end = limit;

    return value >= start && value < end;
}

// Windows are resolved once per scanline into a per-pixel set of layers. The
// regions are tried in a fixed order -- window 0, then window 1, then the
// object window, then everything outside all of them -- and the first match
// decides the pixel, so an overlap belongs to the earlier window.
static void compute_window_mask(int line, uint16_t dispcnt)
{
    uint16_t winin, winout;
    uint8_t inside0, inside1, inside_obj, outside;
    int win0 = (dispcnt & DISPCNT_WIN0) != 0;
    int win1 = (dispcnt & DISPCNT_WIN1) != 0;
    int win_obj = (dispcnt & DISPCNT_WIN_OBJ) != 0;
    int row0, row1;

    // With no window enabled there is no masking at all, which is the state
    // every frame drawn before windows existed was in.
    if (!win0 && !win1 && !win_obj)
    {
        memset(window_mask, WINDOW_EVERYTHING, sizeof(window_mask));
        return;
    }

    winin = io16(REG_WININ);
    winout = io16(REG_WINOUT);
    inside0 = winin & WINDOW_CONTROL_MASK;
    inside1 = (winin >> 8) & WINDOW_CONTROL_MASK;
    outside = winout & WINDOW_CONTROL_MASK;
    inside_obj = (winout >> 8) & WINDOW_CONTROL_MASK;

    row0 = win0 && window_contains(io16(REG_WIN0V), line - view_oy(), NATIVE_H);
    row1 = win1 && window_contains(io16(REG_WIN0V + 2), line - view_oy(), NATIVE_H);

    for (int x = 0; x < screen_w; x++)
    {
        if (row0 && window_contains(io16(REG_WIN0H), x - view_ox(), NATIVE_W))
            window_mask[x] = inside0;
        else if (row1 && window_contains(io16(REG_WIN0H + 2), x - view_ox(), NATIVE_W))
            window_mask[x] = inside1;
        else if (win_obj && obj_window[x])
            window_mask[x] = inside_obj;
        else
            window_mask[x] = outside;
    }
}

// Where an affine layer starts sampling on this scanline. The reference point
// walks down the screen by one column of the matrix per line; the caller walks
// across it by one row per pixel.
static void affine_line_start(int bg, int line, int mos_v, int32_t *x, int32_t *y)
{
    int params = REG_AFFINE_BLOCK(bg);
    int16_t pb = (int16_t)io16(params + 2);
    int16_t pd = (int16_t)io16(params + 6);
    int row = mosaic_snap(line, mos_v);

    *x = bg_reference(io32(params + 8)) + (int32_t)pb * row;
    *y = bg_reference(io32(params + 12)) + (int32_t)pd * row;
}

// The bitmap modes replace BG2's tiles and map with a frame buffer, transformed
// by the same matrix. Mode 3 is one full-screen 16-bit frame; mode 4 drops to
// 8-bit paletted and gains a second frame; mode 5 keeps 16-bit and shrinks to
// 160x128 to afford one.
static void render_bitmap_bg_line(int mode, int line)
{
    uint16_t control = io16(REG_BG0CNT + BITMAP_BG * 2);
    uint16_t dispcnt = io16(REG_DISPCNT);
    uint16_t mosaic = io16(REG_MOSAIC);
    int16_t pa = (int16_t)io16(REG_AFFINE_BLOCK(BITMAP_BG));
    int16_t pc = (int16_t)io16(REG_AFFINE_BLOCK(BITMAP_BG) + 4);
    int mos_h = (control & BGCNT_MOSAIC) ? MOSAIC_BG_H(mosaic) : 1;
    int mos_v = (control & BGCNT_MOSAIC) ? MOSAIC_BG_V(mosaic) : 1;
    int width = mode == 5 ? BITMAP_MODE5_W : NATIVE_W;
    int height = mode == 5 ? BITMAP_MODE5_H : NATIVE_H;
    int paletted = mode == 4;
    // Mode 3's frame fills the region on its own, so it has no second one to
    // select between.
    uint32_t base = (mode != 3 && (dispcnt & DISPCNT_FRAME_SELECT)) ? BITMAP_FRAME_SIZE : 0;
    int32_t base_x, base_y;

    affine_line_start(BITMAP_BG, line, mos_v, &base_x, &base_y);

    for (int i = 0; i < screen_w; i++)
    {
        int across = mosaic_snap(i, mos_h);
        int tx = (base_x + (int32_t)pa * across) >> 8;
        int ty = (base_y + (int32_t)pc * across) >> 8;
        uint32_t pixel;

        if (layer_count[i] >= 2 || !(window_mask[i] & (1 << BITMAP_BG)))
            continue;
        // A frame buffer does not wrap: off its edge there is nothing to draw.
        if (tx < 0 || tx >= width || ty < 0 || ty >= height)
            continue;

        pixel = (uint32_t)(ty * width + tx);
        if (paletted)
        {
            uint8_t index = agb_mem.vram[base + pixel];

            if (index == 0)
                continue;
            deposit(i, bg_palette(index), BITMAP_BG);
        }
        else
        {
            // Direct colour: the frame buffer holds BGR555, and every pixel of
            // it is opaque -- there is no index left to mean transparent.
            uint32_t at = base + pixel * 2;

            deposit(i, (uint16_t)(agb_mem.vram[at] | (agb_mem.vram[at + 1] << 8)), BITMAP_BG);
        }
    }
}

// Affine backgrounds are always 8bpp and their maps are one byte per tile --
// no flip bits, no palette bank, so an entry is just a tile number.
static void render_affine_bg_line(int bg, int line)
{
    uint16_t control = io16(REG_BG0CNT + bg * 2);
    int params = REG_AFFINE_BLOCK(bg);
    int16_t pa = (int16_t)io16(params);
    int16_t pc = (int16_t)io16(params + 4);
    int tiles = BGCNT_AFFINE_TILES(control);
    int extent = tiles * 8;
    int wrap = (control & BGCNT_AFFINE_WRAP) != 0;
    const uint8_t *chars = agb_mem.vram + BGCNT_CHAR_BASE(control) * CHAR_BLOCK_SIZE;
    const uint8_t *screen = agb_mem.vram + BGCNT_SCREEN_BASE(control) * SCREEN_BLOCK_SIZE;

    uint16_t mosaic = io16(REG_MOSAIC);
    int mos_h = (control & BGCNT_MOSAIC) ? MOSAIC_BG_H(mosaic) : 1;
    int mos_v = (control & BGCNT_MOSAIC) ? MOSAIC_BG_V(mosaic) : 1;

    int32_t base_x, base_y;

    affine_line_start(bg, line, mos_v, &base_x, &base_y);

    for (int i = 0; i < screen_w; i++)
    {
        int across = mosaic_snap(i, mos_h);
        int tx = (base_x + (int32_t)pa * across) >> 8;
        int ty = (base_y + (int32_t)pc * across) >> 8;
        int colour;

        if (layer_count[i] >= 2 || !(window_mask[i] & (1 << bg)))
            continue;

        if (wrap)
        {
            tx &= extent - 1;
            ty &= extent - 1;
        }
        else if (tx < 0 || tx >= extent || ty < 0 || ty >= extent)
        {
            continue;
        }

        colour = chars[screen[(ty >> 3) * tiles + (tx >> 3)] * TILE_SIZE_8BPP
                       + (ty & 7) * 8 + (tx & 7)];
        if (colour == 0)
            continue;

        deposit(i, bg_palette(colour), bg);
    }
}

// Byte offset of texel (tx, ty) within an object's tile data. 1D mapping lays
// an object's tiles out back to back; 2D mapping cuts them from a fixed
// 32-tile-wide grid, which is the only difference between the two.
static uint32_t obj_texel_offset(int tile, int one_d, int width, int is_256, int tx, int ty)
{
    int tile_size = is_256 ? TILE_SIZE_8BPP : TILE_SIZE_4BPP;
    uint32_t row_stride = one_d ? (uint32_t)(width >> 3) * tile_size : OBJ_2D_ROW_STRIDE;

    return (uint32_t)tile * TILE_SIZE_4BPP
           + (uint32_t)(ty >> 3) * row_stride
           + (uint32_t)(tx >> 3) * tile_size
           + (uint32_t)(ty & 7) * (is_256 ? 8 : 4);
}

// Palette index for a texel, or 0 for transparent.
static int obj_texel(uint32_t offset, int is_256, int tx, int palette)
{
    int index;

    if (is_256)
        return obj_vram(offset + (tx & 7));

    uint8_t pair = obj_vram(offset + ((tx & 7) >> 1));

    index = (tx & 1) ? (pair >> 4) : (pair & 0xF);
    return index ? index + palette * 16 : 0;
}

// Objects are resolved into a line buffer ahead of the backgrounds, because
// which object owns a pixel is settled among the objects alone: lowest priority
// value wins, and OAM order breaks a tie. Only then does that pixel compete
// with the backgrounds, at the priority it carries.
static void render_obj_line(int line)
{
    uint16_t dispcnt = io16(REG_DISPCNT);
    uint16_t mosaic = io16(REG_MOSAIC);
    int one_d = (dispcnt & DISPCNT_OBJ_1D_MAP) != 0;
    int min_tile = (dispcnt & DISPCNT_MODE_MASK) >= 3 ? OBJ_BITMAP_MIN_TILE : 0;

    memset(obj_colour, 0, sizeof(obj_colour));
    memset(obj_window, 0, sizeof(obj_window));
    memset(obj_semi, 0, sizeof(obj_semi));

    if (!(dispcnt & DISPCNT_OBJ_ENABLE))
        return;

    for (int n = 0; n < OBJ_COUNT; n++)
    {
        uint16_t attr0 = oam16(n * 8);
        uint16_t attr1 = oam16(n * 8 + 2);
        uint16_t attr2 = oam16(n * 8 + 4);
        int mode = OBJ_MODE(attr0);
        int shape = OBJ_SHAPE(attr0);
        int width = obj_dimensions[shape][OBJ_SIZE(attr1)][0];
        int height = obj_dimensions[shape][OBJ_SIZE(attr1)][1];
        int is_256 = (attr0 & OBJ_256_COLOUR) != 0;
        int prio = OBJ_PRIORITY(attr2);
        int palette = OBJ_PALETTE(attr2);
        int tile = OBJ_TILE_INDEX(attr2);
        int affine = mode != OBJ_MODE_NORMAL;
        // Double size grows the area the object may draw into, not the object:
        // a rotated sprite needs the corners its own box cannot hold.
        int box_w = mode == OBJ_MODE_AFFINE_DOUBLE ? width * 2 : width;
        int box_h = mode == OBJ_MODE_AFFINE_DOUBLE ? height * 2 : height;
        int x = OBJ_X(attr1);
        // Objects wrap at 256 rather than at the bottom of the screen, so one
        // placed low enough reappears at the top.
        int py = (line - OBJ_Y(attr0)) & 0xFF;
        int16_t pa = 0, pb = 0, pc = 0, pd = 0;

        // A window object contributes shape rather than colour: its opaque
        // texels mark out a region, and it is never drawn.
        int is_window = OBJ_GFX_MODE(attr0) == OBJ_GFX_WINDOW;
        int is_semi = OBJ_GFX_MODE(attr0) == OBJ_GFX_SEMI;
        // Objects carry their own mosaic sizes, in the upper half of the register.
        int mos_h = (attr0 & OBJ_MOSAIC) ? MOSAIC_OBJ_H(mosaic) : 1;
        int mos_v = (attr0 & OBJ_MOSAIC) ? MOSAIC_OBJ_V(mosaic) : 1;

        if (mode == OBJ_MODE_HIDDEN)
            continue;
        if (py >= box_h || tile < min_tile)
            continue;

        if (x >= 0x100)
            x -= 0x200;

        // Mosaic works in the object's own space, so it is applied to the
        // object-relative coordinate before a flip turns it around.
        py = mosaic_snap(py, mos_v);

        if (affine)
        {
            int group = OBJ_AFFINE_GROUP(attr1);

            pa = (int16_t)oam16(OBJ_AFFINE_PARAM(group, 0));
            pb = (int16_t)oam16(OBJ_AFFINE_PARAM(group, 1));
            pc = (int16_t)oam16(OBJ_AFFINE_PARAM(group, 2));
            pd = (int16_t)oam16(OBJ_AFFINE_PARAM(group, 3));
        }
        else if (attr1 & OBJ_VFLIP)
        {
            py = height - 1 - py;
        }

        for (int col = 0; col < box_w; col++)
        {
            int sx = x + col + view_ox();
            int across = mosaic_snap(col, mos_h);
            int tx, ty;
            int index;

            if (sx < 0 || sx >= screen_w)
                continue;
            // A window object is not competing for the pixel, so the occlusion
            // test does not apply to it.
            if (!is_window && obj_colour[sx] && obj_prio[sx] <= prio)
                continue;

            if (affine)
            {
                // Screen offset from the centre of the box, transformed into
                // texture space and re-centred on the object itself.
                int dx = across - box_w / 2;
                int dy = py - box_h / 2;

                tx = ((pa * dx + pb * dy) >> OBJ_AFFINE_FRACTION) + width / 2;
                ty = ((pc * dx + pd * dy) >> OBJ_AFFINE_FRACTION) + height / 2;

                if (tx < 0 || tx >= width || ty < 0 || ty >= height)
                    continue;
            }
            else
            {
                tx = (attr1 & OBJ_HFLIP) ? width - 1 - across : across;
                ty = py;
            }

            index = obj_texel(obj_texel_offset(tile, one_d, width, is_256, tx, ty),
                              is_256, tx, palette);
            if (index == 0)
                continue;

            if (is_window)
            {
                obj_window[sx] = 1;
                continue;
            }

            obj_colour[sx] = (uint8_t)index;
            obj_prio[sx] = (uint8_t)prio;
            obj_semi[sx] = (uint8_t)is_semi;
        }
    }
}

static void blit_obj_line(int priority)
{
    for (int x = 0; x < screen_w; x++)
    {
        if (!obj_colour[x] || obj_prio[x] != priority || layer_count[x] >= 2)
            continue;
        if (!(window_mask[x] & WINDOW_OBJ))
            continue;

        deposit(x, obj_palette(obj_colour[x]), LAYER_OBJ);
    }
}

// The blend coefficients are five bits holding a value of 0..16, so anything
// above 16 means the same as 16.
static int bld_coefficient(uint16_t reg, int shift)
{
    int value = (reg >> shift) & 0x1F;

    return value > 16 ? 16 : value;
}

static int clamp31(int channel)
{
    return channel > 31 ? 31 : channel;
}

static uint16_t blend_alpha(uint16_t top, uint16_t bottom, int eva, int evb)
{
    int r = clamp31((BGR_R(top) * eva + BGR_R(bottom) * evb) >> 4);
    int g = clamp31((BGR_G(top) * eva + BGR_G(bottom) * evb) >> 4);
    int b = clamp31((BGR_B(top) * eva + BGR_B(bottom) * evb) >> 4);

    return BGR(r, g, b);
}

// Towards white or towards black, by the same fraction of the distance.
static uint16_t blend_brightness(uint16_t colour, int evy, int brighten)
{
    int r = BGR_R(colour);
    int g = BGR_G(colour);
    int b = BGR_B(colour);

    if (brighten)
    {
        r += ((31 - r) * evy) >> 4;
        g += ((31 - g) * evy) >> 4;
        b += ((31 - b) * evy) >> 4;
    }
    else
    {
        r -= (r * evy) >> 4;
        g -= (g * evy) >> 4;
        b -= (b * evy) >> 4;
    }

    return BGR(r, g, b);
}

// Turn the two frontmost candidates at each pixel into the colour that ships.
//
// A semi-transparent object asks to be blended whatever the effect register
// selects, and outranks it -- but only where something underneath is a second
// target. Where that fails it falls back to whatever the register wanted, which
// is what hardware does with it.
static void resolve_line(int line, uint16_t backdrop)
{
    uint32_t *out = framebuffer + line * screen_w;
    uint16_t bldcnt = io16(REG_BLDCNT);
    uint16_t bldalpha = io16(REG_BLDALPHA);
    int effect = BLD_EFFECT(bldcnt);
    int first = BLD_FIRST_TARGET(bldcnt);
    int second = BLD_SECOND_TARGET(bldcnt);
    int eva = bld_coefficient(bldalpha, 0);
    int evb = bld_coefficient(bldalpha, 8);
    int evy = bld_coefficient(io16(REG_BLDY), 0);

    for (int x = 0; x < screen_w; x++)
    {
        int count = layer_count[x];
        uint16_t top = count > 0 ? layer_colour[0][x] : backdrop;
        int top_id = count > 0 ? layer_id[0][x] : LAYER_BACKDROP;
        uint16_t under = count > 1 ? layer_colour[1][x] : backdrop;
        // Nothing sits under the backdrop, so a pixel it already owns has no
        // second layer to blend with.
        int under_id = count > 1 ? layer_id[1][x] : (count > 0 ? LAYER_BACKDROP : LAYER_NONE);
        int semi = top_id == LAYER_OBJ && obj_semi[x];
        int blended = 0;

        if (window_mask[x] & WINDOW_EFFECT)
        {
            int under_is_target = under_id != LAYER_NONE && (second & (1 << under_id));

            if (semi && under_is_target)
            {
                top = blend_alpha(top, under, eva, evb);
                blended = 1;
            }

            if (!blended && (first & (1 << top_id)))
            {
                if (effect == BLD_ALPHA && under_is_target)
                    top = blend_alpha(top, under, eva, evb);
                else if (effect == BLD_BRIGHTEN)
                    top = blend_brightness(top, evy, 1);
                else if (effect == BLD_DARKEN)
                    top = blend_brightness(top, evy, 0);
            }
        }

        out[x] = to_argb(top);
    }
}

// A scanline begins: the line counter moves, and a game watching for one
// particular line hears about it here.
static void scanline_begin(int line)
{
    uint16_t dispstat = io16(REG_DISPSTAT) & ~(DISPSTAT_HBLANK_FLAG | DISPSTAT_VCOUNT_FLAG);

    io16_write(REG_VCOUNT, (uint16_t)line);
    if (DISPSTAT_VCOUNT_SETTING(dispstat) == line)
        dispstat |= DISPSTAT_VCOUNT_FLAG;
    io16_write(REG_DISPSTAT, dispstat);

    if ((dispstat & DISPSTAT_VCOUNT_FLAG) && (dispstat & DISPSTAT_VCOUNT_IRQ))
        agb_irq_raise(AGB_IRQ_VCOUNT);
}

// And ends. The handler runs after the line is drawn and before the next one,
// which is the window a per-scanline effect writes its registers in -- every
// layer re-reads them per line, so the write lands on the line after this.
static void scanline_end(void)
{
    uint16_t dispstat = io16(REG_DISPSTAT);

    io16_write(REG_DISPSTAT, dispstat | DISPSTAT_HBLANK_FLAG);

    // Before the handler: a channel armed for H-blank outranks the CPU, so what
    // it writes is already there when the handler runs and can be overwritten by
    // it, which is the order the hardware gives them.
    agb_dma_trigger(AGB_DMA_START_HBLANK);

    if (dispstat & DISPSTAT_HBLANK_IRQ)
        agb_irq_raise(AGB_IRQ_HBLANK);
}

void agb_ppu_render_frame(void)
{
    frame_serial++;

    for (int line = 0; line < screen_h; line++)
    {
        // Read per scanline rather than per frame: a per-scanline effect may
        // have changed any of this from the previous line's H-blank.
        uint16_t dispcnt;
        int mode;

        scanline_begin(line);
        dispcnt = io16(REG_DISPCNT);
        mode = dispcnt & DISPCNT_MODE_MASK;

        if (dispcnt & DISPCNT_FORCED_BLANK)
        {
            uint32_t *blank = framebuffer + line * screen_w;

            for (int x = 0; x < screen_w; x++)
                blank[x] = 0x00FFFFFF;
            scanline_end();
            continue;
        }

        memset(layer_count, 0, sizeof(layer_count));
        // The object pass first: the object window is a region built from the
        // shapes of objects, so the mask cannot be resolved before it.
        render_obj_line(line);
        compute_window_mask(line, dispcnt);

        // Front to back, so the first layer to claim a pixel is the frontmost:
        // priority 0 is nearest, and among equal priorities objects sit above
        // every background and the lower BG number wins.
        for (int priority = 0; priority < 4; priority++)
        {
            blit_obj_line(priority);

            for (int bg = 0; bg < 4; bg++)
            {
                // Mode 0 is four text layers; mode 1 is two text plus BG2
                // affine; mode 2 is BG2 and BG3 affine. A layer a mode does not
                // define is not drawn at all.
                int bitmap = mode >= 3 && mode <= 5;
                int affine = (mode == 1 && bg == 2) || (mode == 2 && bg >= 2);
                uint16_t control;

                if (!(dispcnt & DISPCNT_BG_ENABLE(bg)))
                    continue;

                if (mode == 1 && bg == 3)
                    continue;
                if (mode == 2 && bg < 2)
                    continue;
                if (bitmap && bg != BITMAP_BG)
                    continue;
                // Modes 6 and 7 are prohibited and define no layer at all.
                if (mode >= 3 && !bitmap)
                    continue;

                control = io16(REG_BG0CNT + bg * 2);
                if ((control & BGCNT_PRIORITY_MASK) != priority)
                    continue;

                if (bitmap)
                    render_bitmap_bg_line(mode, line);
                else if (affine)
                    render_affine_bg_line(bg, line);
                else
                    render_text_bg_line(bg, line);
            }
        }

        resolve_line(line, bg_palette(0));
        scanline_end();
    }
}
