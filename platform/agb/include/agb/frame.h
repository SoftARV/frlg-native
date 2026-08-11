#ifndef GUARD_AGB_FRAME_H
#define GUARD_AGB_FRAME_H

#include <stdint.h>

// Phase 1 frame driver. The game blocks in VBlankIntrWait from deep inside
// nested code, so the only way out is a non-local jump. Phase 2 replaces this
// with the fiber loop in docs/adr/0004-fiber-frame-loop.md, which can also run
// HBlank handlers between scanlines.
uint32_t agb_frame_run(void (*entry)(void), uint32_t max_frames);

uint32_t agb_frame_count(void);

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

// Called from the game's V-blank spin. Nothing in real time; the frame boundary
// in lockstep.
void agb_frame_idle(void);

#endif // GUARD_AGB_FRAME_H
