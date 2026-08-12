#include <string.h>

#include "agb/dma.h"
#include "agb/memmap.h"

#define DMA_REG_BASE 0xB0
#define DMA_REG_STRIDE 12

#define DEST_MODE 0x0060
#define DEST_INC 0x0000
#define DEST_DEC 0x0020
#define DEST_FIXED 0x0040
#define DEST_RELOAD 0x0060
#define SRC_MODE 0x0180
#define SRC_DEC 0x0080
#define SRC_FIXED 0x0100
#define REPEAT 0x0200
#define UNIT_32BIT 0x0400
#define START_MASK 0x3000
#define START_SHIFT 12
#define ENABLE 0x8000

// A count of zero means the maximum, which differs by channel: 0x4000 units for
// DMA3 and 0x4000 for the rest on this hardware's terms, since only DMA3 has the
// wider counter.
#define COUNT_MAX(channel) ((channel) == 3 ? 0x10000u : 0x4000u)

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

// One transfer of a channel armed to wait for something. Hardware latches the
// addresses when the channel is enabled and walks its own copies; the register
// file is walked in place here instead, which comes to the same thing because
// those registers are write-only -- nothing can see the difference, and a
// re-arm rewrites them anyway.
static void run_timed_channel(int channel, uint32_t flags, uint32_t count)
{
    volatile uint32_t *regs = dma_regs(channel);
    uint32_t unit = (flags & UNIT_32BIT) ? 4 : 2;
    uint8_t *dest = (uint8_t *)(uintptr_t)regs[1];
    const uint8_t *src = (const uint8_t *)(uintptr_t)regs[0];
    uint32_t moved;

    if (count == 0)
        count = COUNT_MAX(channel);
    moved = count * unit;

    for (uint32_t i = 0; i < count; i++)
    {
        memcpy(dest, src, unit);
        if ((flags & SRC_MODE) != SRC_FIXED)
            src += (flags & SRC_MODE) == SRC_DEC ? -(int32_t)unit : (int32_t)unit;
        if ((flags & DEST_MODE) != DEST_FIXED)
            dest += (flags & DEST_MODE) == DEST_DEC ? -(int32_t)unit : (int32_t)unit;
    }

    if (!(flags & REPEAT))
    {
        regs[2] &= ~((uint32_t)ENABLE << 16);
        return;
    }

    // A repeating channel keeps the source where it left off -- that is what
    // feeds a scanline effect its next entry -- while a destination in reload
    // mode goes back to where it started, which is what feeds one register.
    if ((flags & SRC_MODE) != SRC_FIXED)
        regs[0] = (uint32_t)(uintptr_t)src;
    if ((flags & DEST_MODE) == DEST_INC || (flags & DEST_MODE) == DEST_DEC)
        regs[1] = (uint32_t)(uintptr_t)dest;
    (void)moved;
}

// Hardware runs a channel when the thing it waits for happens: the start of
// V-blank, or the end of each visible scanline. The battle transitions are built
// on the second -- one 16-bit write per line into WIN0H from a 160-entry buffer,
// which is what sweeps the window across the screen -- and so is everything
// scanline_effect.c does. Channels are served lowest number first, as their
// priority runs.
void agb_dma_trigger(int timing)
{
    for (int channel = 0; channel < 4; channel++)
    {
        uint32_t control = dma_regs(channel)[2];
        uint32_t flags = control >> 16;

        if (!(flags & ENABLE))
            continue;
        if ((int)((flags & START_MASK) >> START_SHIFT) != timing)
            continue;

        run_timed_channel(channel, flags, control & 0xFFFF);
    }
}
