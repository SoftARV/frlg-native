#ifndef GUARD_AGB_FRAME_H
#define GUARD_AGB_FRAME_H

#include <stdint.h>

// Phase 1 frame driver. The game blocks in VBlankIntrWait from deep inside
// nested code, so the only way out is a non-local jump. Phase 2 replaces this
// with the fiber loop in docs/adr/0004-fiber-frame-loop.md, which can also run
// HBlank handlers between scanlines.
uint32_t agb_frame_run(void (*entry)(void), uint32_t max_frames);

uint32_t agb_frame_count(void);

#endif // GUARD_AGB_FRAME_H
