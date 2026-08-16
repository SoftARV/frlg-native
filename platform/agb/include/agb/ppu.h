#ifndef GUARD_AGB_PPU_H
#define GUARD_AGB_PPU_H

#include <stdbool.h>
#include <stdint.h>

// The renderer is resolution-parametric from the first line of code: widescreen
// is cheap now and a rewrite to retrofit. Nothing below hardcodes 240 or 160.
int agb_ppu_width(void);
int agb_ppu_height(void);

// The viewport is a window onto the backgrounds the game already draws, so the
// ceiling is not this renderer's to choose: it is however much map the field
// keeps drawn. That is 64x32 tiles -- 512x256 pixels -- since the map layers
// were widened and the camera taught to fill them.
//
// The window that scrolls over those 512x256 is smaller than they are, and by
// more than the arithmetic of "512 minus the scroll" suggests.
//
// The camera does not redraw the map as it moves. It rotates: the row leaving
// one edge is the row rewritten for the other, so at every metatile step one
// row and one column of the buffer briefly hold the *far* side's new content.
// The view has to be small enough that those two are off screen when it
// happens, or a strip of the map ahead appears at the edge behind -- which is
// what "walking left or down leaves artifacts at the edge" was.
//
// So the view has to leave a whole metatile spare on *each* side of both axes,
// and where that spare sits is fixed by the camera's scroll offsets rather than
// being split evenly. Horizontally the offset is 8 metatiles (VIEW_SCROLL_X):
// the room left of the view is 128 - (W - 240) / 2, which is a metatile only up
// to W = 464 -- at 480 it is half of one, and the column written for the right
// edge spills eight pixels into the left of the screen. Vertically the offset is
// two metatiles and a half, and 192 is where the view ends on one of the fill
// grid's own lines rather than between two of them.
//
// That last part is where the reasoning runs out. Ending on a line should leave
// the rewritten row whole and off screen, and it does not: walking up still
// draws about eight pixels of somewhere else along the bottom, for the one step
// it takes to cover them. 240, 224, 208 and 192 have all been played or
// measured; the strip shrinks with the ceiling and does not go away. So the
// height is not the lever, and issue #12 holds what is known -- including that
// the two detectors written for it both look for the wrong thing.
//
// Both were measured rather than derived -- a walk in each direction, looking
// for a strip that fails to scroll with the rest of the frame. Raising either
// brings the artifact back at that edge only, and only while walking.
//
// **The height does not go up by making the map layers taller.** 64x64 tiles
// would give the camera twice the rows and put the rewritten one well out of
// sight, and there is nowhere to put them: the field's tilesets fill 0x0000 to
// 0x7FFF exactly, BG0 takes 0x8000 for its own tiles and 0xF800 for its map, and
// the 14 KB left between them holds three 4 KB layers but not three of 8 KB.
//
// Behind that is a wall no amount of VRAM moves. An object's Y is eight bits, so
// everything the hardware can place vertically lives in 256 pixels, and a view
// taller than that draws the same NPC twice. Allowing for where objects have to
// sit before they are seen, the real ceiling on this axis is somewhere near 240
// -- 48 pixels above where it is now, for a change that touches the camera, the
// tilemaps, both wrap points and the VRAM map.
//
// The floor is the hardware's own size, because showing less than a Game Boy
// Advance did would hide things the game put on screen.
#define AGB_PPU_MIN_W 240
#define AGB_PPU_MIN_H 160
#define AGB_PPU_MAX_W 464
#define AGB_PPU_MAX_H 192

// Read a background from the game's own tilemap buffer instead of from VRAM.
//
// A text background is one of four sizes because two bits in a register say so.
// The game does not think in those two bits: it builds its map into a plain
// array in its own heap and copies it into the shape the hardware wants. Given
// the array, this renderer can read it at whatever size it actually is, and the
// copy -- and the four sizes, and the two-block layout a 64-tile-wide map has to
// be written in -- stop applying. See ADR 0024.
//
// `tilemap` is entries of two bytes, row-major, `width` by `height` tiles, and
// wraps on both axes like the hardware's own. It must outlive the frames it is
// used for. NULL puts the background back on VRAM, which is where every one of
// them starts: this is opt-in, per background, so a screen that has not asked
// for it keeps exactly the machine it had.
void agb_ppu_set_bg_source(int bg, const void *tilemap, int width, int height);

// Clamped to the range above. Returns true if the viewport is now different
// from what it was, which is the caller's cue to resize anything it keeps in
// step with it.
bool agb_ppu_set_viewport(int width, int height);

// Compose one frame from the current register, palette, VRAM and OAM state.
void agb_ppu_render_frame(void);

// XRGB8888, agb_ppu_width() * agb_ppu_height() pixels.
const uint32_t *agb_ppu_framebuffer(void);

// Increments once per composed frame, so a reader can tell frames apart.
uint32_t agb_ppu_frame_serial(void);

#endif // GUARD_AGB_PPU_H
