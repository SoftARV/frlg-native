#ifndef GUARD_AGB_M4A_H
#define GUARD_AGB_M4A_H

#include <stdbool.h>

#include "gba/m4a_internal.h"

// The sound mixer, replacing upstream's m4a_1.s. The sequencer above it
// (upstream's m4a.c) is C already and is used as it stands.
//
// Translated from the ARM rather than rewritten, so the result can be checked
// against a known-good emulator sample for sample. Where the original relies on
// something a reader would call a mistake, the comment says so and the code
// keeps the behaviour.

// One channel's envelope for one frame: advances attack, decay, sustain,
// release and the pseudo-echo tail, then folds the master and per-side volumes
// into the values the mixing loop reads.
//
// Returns false when the channel has finished and must not be mixed. Declared
// here because the tests drive it directly; SoundMainRAM is its only other
// caller.
bool agb_m4a_envelope_step(struct SoundInfo *info, struct SoundChannel *chan);

#endif // GUARD_AGB_M4A_H
