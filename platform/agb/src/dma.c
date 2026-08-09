#include <string.h>

#include "agb/dma.h"
#include "agb/memmap.h"

#define DMA_REG_BASE 0xB0
#define DMA_REG_STRIDE 12

#define DEST_MODE 0x0060
#define DEST_FIXED 0x0040
#define SRC_FIXED 0x0100
#define UNIT_32BIT 0x0400
#define START_MASK 0x3000
#define ENABLE 0x8000

static volatile uint32_t *dma_regs(int channel)
{
    return (volatile uint32_t *)(agb_mem.io + DMA_REG_BASE + DMA_REG_STRIDE * channel);
}

void agb_dma_set(int channel, const void *src, void *dest, uint32_t control)
{
    volatile uint32_t *regs = dma_regs(channel);
    uint32_t flags = control >> 16;
    uint32_t count = control & 0xFFFF;
    uint32_t unit;

    regs[0] = (uint32_t)(uintptr_t)src;
    regs[1] = (uint32_t)(uintptr_t)dest;
    regs[2] = control;

    if (!(flags & ENABLE))
        return;

    // Timed channels are driven by the frame loop, not by the write that arms them.
    if (flags & START_MASK)
        return;

    unit = (flags & UNIT_32BIT) ? 4 : 2;

    if (flags & SRC_FIXED)
    {
        uint8_t *out = dest;
        uint32_t i;

        for (i = 0; i < count; i++)
        {
            memcpy(out, src, unit);
            out += unit;
        }
    }
    else if ((flags & DEST_MODE) == DEST_FIXED)
    {
        const uint8_t *in = src;
        uint32_t i;

        for (i = 0; i < count; i++)
        {
            memcpy(dest, in, unit);
            in += unit;
        }
    }
    else
    {
        memcpy(dest, src, count * unit);
    }

    regs[2] = control & ~((uint32_t)ENABLE << 16);
}

void agb_dma_stop(int channel)
{
    dma_regs(channel)[2] = 0;
}
