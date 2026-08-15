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
// keeps drawn. That is 64x32 tiles now -- 512x256 pixels -- since the map
// layers were widened and the camera taught to fill them. Asking for more shows
// the opposite edge wrapped round, not more of the world.
//
// The floor is the hardware's own size, because showing less than a Game Boy
// Advance did would hide things the game put on screen.
#define AGB_PPU_MIN_W 240
#define AGB_PPU_MIN_H 160
#define AGB_PPU_MAX_W 512
#define AGB_PPU_MAX_H 256

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
