// Scanline renderer.
//
// Phase 3 scope: text-mode backgrounds and objects, both regular and affine.
// Affine backgrounds, windows and blending follow, in the order the game
// stresses them. Anything not yet implemented is skipped rather than
// approximated, so a missing feature reads as absent rather than as a subtly
// wrong picture.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#define SCREEN_W 240
#define SCREEN_H 160

#define REG_DISPCNT 0x000
#define REG_BG0CNT 0x008
#define REG_BG0HOFS 0x010

#define DISPCNT_MODE_MASK 0x0007
#define DISPCNT_OBJ_1D_MAP 0x0040
#define DISPCNT_FORCED_BLANK 0x0080
#define DISPCNT_BG_ENABLE(n) (0x0100 << (n))
#define DISPCNT_OBJ_ENABLE 0x1000

#define BGCNT_PRIORITY_MASK 0x0003
#define BGCNT_CHAR_BASE(c) (((c) >> 2) & 3)
#define BGCNT_MOSAIC 0x0040
#define BGCNT_256_COLOUR 0x0080
#define BGCNT_SCREEN_BASE(c) (((c) >> 8) & 0x1F)
#define BGCNT_SIZE(c) (((c) >> 14) & 3)

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
#define OBJ_GFX_WINDOW 2
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

static uint32_t framebuffer[SCREEN_W * SCREEN_H];
static uint32_t frame_serial;

// One scanline of the object layer, resolved before any background is drawn.
// Index 0 means no object claimed the pixel.
static uint8_t obj_colour[SCREEN_W];
static uint8_t obj_prio[SCREEN_W];

int agb_ppu_width(void)
{
    return SCREEN_W;
}

int agb_ppu_height(void)
{
    return SCREEN_H;
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

static uint16_t oam16(int offset)
{
    return *(const volatile uint16_t *)(agb_mem.oam + offset);
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

static void render_text_bg_line(int bg, int line, uint8_t *coverage)
{
    uint16_t control = io16(REG_BG0CNT + bg * 2);
    int hofs = io16(REG_BG0HOFS + bg * 4) & 0x1FF;
    int vofs = io16(REG_BG0HOFS + bg * 4 + 2) & 0x1FF;
    int size = BGCNT_SIZE(control);
    int is_256 = (control & BGCNT_256_COLOUR) != 0;
    const uint8_t *chars = agb_mem.vram + BGCNT_CHAR_BASE(control) * CHAR_BLOCK_SIZE;
    const uint8_t *screen = agb_mem.vram + BGCNT_SCREEN_BASE(control) * SCREEN_BLOCK_SIZE;

    int width_mask = (size == 1 || size == 3) ? 0x1FF : 0xFF;
    int height_mask = (size == 2 || size == 3) ? 0x1FF : 0xFF;
    int src_y = (line + vofs) & height_mask;
    uint32_t *out = framebuffer + line * SCREEN_W;

    for (int x = 0; x < SCREEN_W; x++)
    {
        int src_x = (x + hofs) & width_mask;
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

        if (coverage[x])
            continue;

        out[x] = to_argb(bg_palette(colour));
        coverage[x] = 1;
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
    int one_d = (dispcnt & DISPCNT_OBJ_1D_MAP) != 0;
    int min_tile = (dispcnt & DISPCNT_MODE_MASK) >= 3 ? OBJ_BITMAP_MIN_TILE : 0;

    memset(obj_colour, 0, sizeof(obj_colour));

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

        if (mode == OBJ_MODE_HIDDEN)
            continue;
        // A window object contributes a mask rather than pixels, and it is
        // never drawn on hardware either -- skipping it is exact, not partial.
        if (OBJ_GFX_MODE(attr0) == OBJ_GFX_WINDOW)
            continue;
        if (py >= box_h || tile < min_tile)
            continue;

        if (x >= 0x100)
            x -= 0x200;

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
            int sx = x + col;
            int tx, ty;
            int index;

            if (sx < 0 || sx >= SCREEN_W)
                continue;
            if (obj_colour[sx] && obj_prio[sx] <= prio)
                continue;

            if (affine)
            {
                // Screen offset from the centre of the box, transformed into
                // texture space and re-centred on the object itself.
                int dx = col - box_w / 2;
                int dy = py - box_h / 2;

                tx = ((pa * dx + pb * dy) >> OBJ_AFFINE_FRACTION) + width / 2;
                ty = ((pc * dx + pd * dy) >> OBJ_AFFINE_FRACTION) + height / 2;

                if (tx < 0 || tx >= width || ty < 0 || ty >= height)
                    continue;
            }
            else
            {
                tx = (attr1 & OBJ_HFLIP) ? width - 1 - col : col;
                ty = py;
            }

            index = obj_texel(obj_texel_offset(tile, one_d, width, is_256, tx, ty),
                              is_256, tx, palette);
            if (index == 0)
                continue;

            obj_colour[sx] = (uint8_t)index;
            obj_prio[sx] = (uint8_t)prio;
        }
    }
}

static void blit_obj_line(int line, int priority, uint8_t *coverage)
{
    uint32_t *out = framebuffer + line * SCREEN_W;

    for (int x = 0; x < SCREEN_W; x++)
    {
        if (!obj_colour[x] || obj_prio[x] != priority || coverage[x])
            continue;

        out[x] = to_argb(obj_palette(obj_colour[x]));
        coverage[x] = 1;
    }
}

void agb_ppu_render_frame(void)
{
    uint16_t dispcnt = io16(REG_DISPCNT);
    int mode = dispcnt & DISPCNT_MODE_MASK;

    frame_serial++;

    if (dispcnt & DISPCNT_FORCED_BLANK)
    {
        for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
            framebuffer[i] = 0x00FFFFFF;
        return;
    }

    for (int line = 0; line < SCREEN_H; line++)
    {
        uint32_t backdrop = to_argb(bg_palette(0));
        uint32_t *out = framebuffer + line * SCREEN_W;
        uint8_t coverage[SCREEN_W];

        for (int x = 0; x < SCREEN_W; x++)
            out[x] = backdrop;
        memset(coverage, 0, sizeof(coverage));
        render_obj_line(line);

        // Front to back, so the first layer to claim a pixel keeps it: priority
        // 0 is nearest, and among equal priorities objects sit above every
        // background and the lower BG number wins.
        for (int priority = 0; priority < 4; priority++)
        {
            blit_obj_line(line, priority, coverage);

            for (int bg = 0; bg < 4; bg++)
            {
                uint16_t control;

                if (!(dispcnt & DISPCNT_BG_ENABLE(bg)))
                    continue;

                // Only text backgrounds so far. In modes 1 and 2 the upper
                // layers are affine and are skipped until they are written.
                if (mode == 1 && bg >= 2)
                    continue;
                if (mode == 2 && bg < 2)
                    continue;
                if (mode >= 3)
                    continue;

                control = io16(REG_BG0CNT + bg * 2);
                if ((control & BGCNT_PRIORITY_MASK) != priority)
                    continue;

                render_text_bg_line(bg, line, coverage);
            }
        }
    }
}
