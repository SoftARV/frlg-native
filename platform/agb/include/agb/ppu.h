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
// 480 across: the buffer is 32 metatiles and the recycled column needs two of
// them, because the scroll is only metatile-aligned at the instant of the step.
// 208 down: the buffer is 16 metatiles and the camera's own half-metatile
// offset (VIEW_SCROLL_Y in field_camera.patch) costs one more.
//
// Both were measured rather than derived -- a walk in each direction, looking
// for a strip that fails to scroll with the rest of the frame. Raising either
// brings the artifact back at that edge only, and only while walking.
//
// The floor is the hardware's own size, because showing less than a Game Boy
// Advance did would hide things the game put on screen.
#define AGB_PPU_MIN_W 240
#define AGB_PPU_MIN_H 160
#define AGB_PPU_MAX_W 480
#define AGB_PPU_MAX_H 208

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
