#ifndef GUARD_AGB_FRAME_H
#define GUARD_AGB_FRAME_H

#include <stdint.h>

// Phase 1 frame driver. The game blocks in VBlankIntrWait from deep inside
// nested code, so the only way out is a non-local jump. Phase 2 replaces this
// with the fiber loop in docs/adr/0004-fiber-frame-loop.md, which can also run
// HBlank handlers between scanlines.
uint32_t agb_frame_run(void (*entry)(void), uint32_t max_frames);

uint32_t agb_frame_count(void);

// Ask the game to stop at the next frame boundary. AgbMain never returns on its
// own, so this is how a port closes: agb_frame_run comes back and its thread can
// be joined.
void agb_frame_stop(void);

// Leave the run because game code faulted. Called from a signal handler, so it
// does one thing: the non-local jump back to agb_frame_run, which then returns
// normally. Inert before the run starts, where there is nothing to jump to.
void agb_frame_abort(void);

// Where the key register's contents come from, if not from a keyboard.
//
// A replayed run has to be deterministic, and reading a keyboard from the
// presenting thread is not: the write can land either side of the game's own
// read. So a source is called here instead, on the game's thread, at the same
// point in every frame -- before the V-blank handler the game samples keys in.
// It is handed the frame number, which is what a trace is indexed by.
//
// Active-low, like the register: a set bit is a released key. Null means the
// port is driving the register itself.
void agb_frame_set_key_source(uint16_t (*source)(uint32_t frame));

// Advance frames from the game's own idle point rather than from a wall-clock
// timer. A run is then reproducible -- the same binary reaches the same frame
// with the same contents however loaded the machine is -- which is what the
// golden, audio and trace harnesses compare. It also runs as fast as the host
// allows, since nothing waits out a frame period. See docs/adr/0013-lockstep-capture-clock.md.
void agb_frame_set_lockstep(int on);

// In lockstep, wait out the rest of the frame period before advancing, so the
// game keeps real time without the frame boundary depending on how long the
// work took. What a recording wants: playable, and still one frame per step.
void agb_frame_set_pace(int on);

// How many frames the watchdog had to advance because the game was spinning
// somewhere that never reaches the idle hook. Zero means the run was driven
// entirely by the game itself. See docs/adr/0014-lockstep-stall-watchdog.md.
uint32_t agb_frame_watchdog_ticks(void);

// Called from the game's V-blank spin. Nothing in real time; the frame boundary
// in lockstep.
void agb_frame_idle(void);

// Called on the game's thread the instant the frame is composed, for a port that
// wants to read memory belonging to the picture it just drew.
//
// Same reason as the key source above, and the same trap in reverse: the game
// writes VRAM, palette and registers from its own thread, so a presenting thread
// that reads them gets whatever moment it happened to catch. That is not a
// sample of the frame -- two runs of the same recording reaching the same frame
// disagreed about VRAM while agreeing pixel for pixel about the picture, which
// is how this was found. A reader that must correspond to the frame reads here.
//
// It is handed the frame number, so a dump can name what it holds.
void agb_frame_set_composed(void (*fn)(uint32_t frame));

#endif // GUARD_AGB_FRAME_H
