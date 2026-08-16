#ifndef GUARD_AGB_PPU_H
#define GUARD_AGB_PPU_H

#include <stdbool.h>
#include <stdint.h>

// The renderer is resolution-parametric from the first line of code: widescreen
// is cheap now and a rewrite to retrofit. Nothing below hardcodes 240 or 160.
int agb_ppu_width(void);
int agb_ppu_height(void);

// The viewport is a window onto the backgrounds the field draws, so the ceiling
// is not this renderer's to choose: it is however much map is kept drawn. That
// is 64x64 tiles, 512x512 pixels, read straight out of the game's own buffer
// rather than from VRAM at one of the four sizes a register can name (ADR 0024).
//
// The window that scrolls over it is smaller, because the camera does not redraw
// the map as it moves -- it rotates. The row leaving one edge is the row
// rewritten for the other, so at every metatile step one row and one column hold
// the far side's new content, and the view has to be small enough that both are
// off screen while it happens. Otherwise a strip of the map ahead appears at the
// edge behind: that was "walking left or down leaves artifacts at the edge", and
// it was issue 12's eight pixels along the bottom, which the second half of the
// buffer's height put out of reach.
//
// Across, the buffer is 512 and the camera's scroll offset decides where the
// spare room sits: 128 - (W - 240) / 2 is a whole metatile only up to W = 464.
//
// Down, the buffer has room to spare and the ceiling is somewhere else entirely.
// **An object's Y is eight bits.** Everything the hardware can place vertically
// lives in 256 pixels, and objects must exist below the view before they are
// seen -- the field spawns them nine metatiles past the player, which reaches
// 248. A view of 240 puts its own top edge 256 pixels above that, which is the
// same place: NPCs standing below the screen were drawn along the top of it, and
// walking towards them made them vanish, because that moved them out of the
// aliasing and not into view.
//
// 192 is where that stays small. Lifting it means giving the renderer the
// position the game computed instead of the eight bits that survived OAM, which
// is the next step of ADR 0024 and what the width is waiting on too.
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
