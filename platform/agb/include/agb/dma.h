#ifndef GUARD_AGB_DMA_H
#define GUARD_AGB_DMA_H

#include <stdint.h>

void agb_dma_set(int channel, const void *src, void *dest, uint32_t control);
void agb_dma_stop(int channel);

#endif // GUARD_AGB_DMA_H
