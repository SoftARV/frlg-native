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
// two metatiles and a half, and that half is what decides the height: the fill
// grid sits eight pixels off the scroll, so a view whose bottom edge does not
// land on one of its lines cuts the rewritten row in half and shows the top of
// it. 208 leaves exactly those eight pixels showing -- a strip of somewhere
// else along the bottom, for the moment it takes the next step to cover it.
// 192 ends on a line.
//
// Both were measured rather than derived -- a walk in each direction, looking
// for a strip that fails to scroll with the rest of the frame. Raising either
// brings the artifact back at that edge only, and only while walking.
//
// The floor is the hardware's own size, because showing less than a Game Boy
// Advance did would hide things the game put on screen.
#define AGB_PPU_MIN_W 240
#define AGB_PPU_MIN_H 160
#define AGB_PPU_MAX_W 464
#define AGB_PPU_MAX_H 192

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
