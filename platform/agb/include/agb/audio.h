// Where mixed audio goes.
//
// The mixer does not call the host itself -- the same arrangement as the PPU,
// which composes into a buffer the port presents -- so the port installs a sink
// and decides what listening means. Kept apart from agb/m4a.h so that a port
// needs none of the sequencer's own types to do it.

#ifndef GUARD_AGB_AUDIO_H
#define GUARD_AGB_AUDIO_H

#include <stdint.h>

// How many output samples each of the mixer's samples becomes. The sampled side
// is held across them, the way the hardware's FIFO holds one; the hardware
// channels are rendered at the higher rate, where their harmonics and sweeps
// still exist. Four puts the output at 53516 Hz.
//
// 13379 Hz is the rate the game runs its FIFOs at, not the rate the machine puts
// out: three quarters of the reference's energy sits above what that rate can
// carry, and handing it over unchanged is a perfect low-pass at 6.7 kHz.
#define AGB_AUDIO_OVERSAMPLE 4

// One frame's worth of 8-bit signed stereo, the two sides held separately the
// way the mixer keeps them. `rate` is the rate the samples are handed over at --
// the mixer's own rate times the oversampling above.
typedef void (*agb_audio_sink)(const int8_t *right, const int8_t *left, int samples, int rate);

void agb_m4a_set_audio_sink(agb_audio_sink sink);

#endif // GUARD_AGB_AUDIO_H
