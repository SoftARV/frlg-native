// Scanline renderer.
//
// Phase 3 scope: text-mode backgrounds. Objects, affine backgrounds, windows
// and blending follow, in the order the game stresses them. Anything not yet
// implemented is skipped rather than approximated, so a missing feature reads
// as absent rather than as a subtly wrong picture.

#include <string.h>

#include "agb/memmap.h"
#include "agb/ppu.h"

#define SCREEN_W 240
#define SCREEN_H 160

#define REG_DISPCNT 0x000
#define REG_BG0CNT 0x008
#define REG_BG0HOFS 0x010

#define DISPCNT_MODE_MASK 0x0007
#define DISPCNT_FORCED_BLANK 0x0080
#define DISPCNT_BG_ENABLE(n) (0x0100 << (n))

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

static uint32_t framebuffer[SCREEN_W * SCREEN_H];
static uint32_t frame_serial;

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

static uint16_t bg_palette(int index)
{
    return *(const volatile uint16_t *)(agb_mem.pltt + index * 2);
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

        // Front to back, so the first layer to claim a pixel keeps it: priority
        // 0 is nearest, and among equal priorities the lower BG number wins.
        for (int priority = 0; priority < 4; priority++)
        {
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
