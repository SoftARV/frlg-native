// The GBA's four hardware sound channels.
//
// The m4a engine mixes direct sound in software; these it drives by writing
// registers, and something has to turn those writes into samples. Two square
// channels, a programmable wave and a noise generator -- the Game Boy's sound
// hardware, carried forward.

#ifndef GUARD_AGB_PSG_H
#define GUARD_AGB_PSG_H

#include <stdint.h>

// Generate `samples` samples at `rate` and add them to the frame's two buffers,
// which already hold the software mixer's output. The registers are read as they
// stand: the game rewrites them every frame through CgbSound.
void agb_psg_mix(int8_t *right, int8_t *left, int samples, int rate);

// Forget every channel's state. Only the tests need this; a running port never
// stops the sound hardware.
void agb_psg_reset(void);

#endif // GUARD_AGB_PSG_H
