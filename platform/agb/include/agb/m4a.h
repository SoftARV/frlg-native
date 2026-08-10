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

// Mix one channel's samples into a frame's two PCM buffers, at the sample rate
// the wave was recorded at -- the fixed-frequency path, taken when the tone
// type has TONEDATA_TYPE_FIX set.
//
// `right` and `left` are the two halves of the sound header's PCM buffer, and
// `samples` is how many the frame wants. The channel's own sample pointer and
// remaining count are advanced, looping if the wave says to.
//
// Returns false when the wave ran out and does not loop: the channel is
// released and the rest of the frame gets nothing from it.
bool agb_m4a_mix_fixed(struct SoundChannel *chan, s8 *right, s8 *left, int samples);

// The pitched path, taken when the tone type does not have TONEDATA_TYPE_FIX:
// the wave is resampled to the output rate, interpolating between neighbouring
// samples. The step comes from the sound header's divFreq times the channel's
// own frequency, and the channel's `fw` carries the fractional position across
// frames.
//
// Same contract as the fixed path otherwise: buffers are accumulated into, the
// wave loops if it says to, and false means the channel has been released.
bool agb_m4a_mix_pitched(const struct SoundInfo *info, struct SoundChannel *chan,
                         s8 *right, s8 *left, int samples);

// The sound header's address, which the sequencer keeps at a fixed spot in
// IWRAM. Both halves of the m4a replacement reach it this way.
struct SoundInfo *agb_sound_info(void);

// Mix every channel into one frame's buffers: prepare them, then step each
// channel's envelope and hand it to whichever path its tone type asks for.
// This is what upstream calls SoundMainRAM, under our own name -- see the
// comment on the definition for why.
void agb_m4a_mix_frame(struct SoundInfo *info, s8 *frame, int samples);

// The reversed path, taken when the tone type has TONEDATA_TYPE_REV: the wave
// is walked from its end towards its start, resampling as the forward pitched
// path does. Covers the fixed-frequency variant too, as a step of exactly one
// sample. A reversed wave does not loop -- running out of samples ends the note.
bool agb_m4a_mix_reversed(const struct SoundInfo *info, struct SoundChannel *chan,
                          s8 *right, s8 *left, int samples);

// Where in the PCM area this frame writes. The area holds several frames and
// the DMA counter says which one is free, so the mixer writes ahead of what the
// hardware is reading out.
s8 *agb_m4a_frame_buffer(const struct SoundInfo *info);

// Prepare that frame's two buffers before any channel is mixed in: either
// cleared, or seeded with a reverb of what is already there. `frame` is the
// right-hand buffer; the left sits PCM_DMA_BUF_SIZE further on.
void agb_m4a_prepare_frame(const struct SoundInfo *info, s8 *frame, int samples);

// Split a note's velocity across the two sides by its pan. Upstream's header
// does not declare this one -- it is reached only through the interpreter -- so
// it is declared here for the tests and for our own callers.
void ChnVolSetAsm(struct SoundChannel *chan, struct MusicPlayerTrack *track);

// Called once per frame from the game's own V-blank handler. Declared in
// upstream's m4a.h rather than its internal header, which our layer does not
// pull in, so it is repeated here.
void m4aSoundVSync(void);

#endif // GUARD_AGB_M4A_H
