#ifndef GUARD_AGB_DMA_H
#define GUARD_AGB_DMA_H

#include <stdint.h>

void agb_dma_set(int channel, const void *src, void *dest, uint32_t control);
void agb_dma_stop(int channel);

// Run every channel armed to wait for this: 1 at the start of V-blank, 2 at the
// end of each visible scanline. Called by the frame driver and the PPU rather
// than by the write that arms the channel.
#define AGB_DMA_START_VBLANK 1
#define AGB_DMA_START_HBLANK 2

void agb_dma_trigger(int timing);

#endif // GUARD_AGB_DMA_H
