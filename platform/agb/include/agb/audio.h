// Where mixed audio goes.
//
// The mixer does not call the host itself -- the same arrangement as the PPU,
// which composes into a buffer the port presents -- so the port installs a sink
// and decides what listening means. Kept apart from agb/m4a.h so that a port
// needs none of the sequencer's own types to do it.

#ifndef GUARD_AGB_AUDIO_H
#define GUARD_AGB_AUDIO_H

#include <stdint.h>

// One frame's worth of 8-bit signed stereo, the two sides held separately the
// way the mixer keeps them. `rate` is the mixer's output rate, which the game
// can change while running through m4aSoundMode.
typedef void (*agb_audio_sink)(const int8_t *right, const int8_t *left, int samples, int rate);

void agb_m4a_set_audio_sink(agb_audio_sink sink);

#endif // GUARD_AGB_AUDIO_H
