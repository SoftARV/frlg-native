#include "agb/irq.h"
#include "agb/memmap.h"

// Declared locally rather than by including the game's main.h, which would drag
// global.h and its whole dependency tree into the hardware layer.
typedef void (*IntrFunc)(void);
extern IntrFunc gIntrTable[];

#define REG_OFF_IE 0x200
#define REG_OFF_IF 0x202
#define REG_OFF_IME 0x208

// gIntrTableTemplate in main.c is ordered by the BIOS vector's scan priority,
// not by IE bit order, so index -> flag needs this mapping.
static const uint16_t agb_irq_flag_for_slot[] = {
    1 << 2,  // V-count
    1 << 7,  // Serial
    1 << 6,  // Timer 3
    1 << 1,  // H-blank
    1 << 0,  // V-blank
    1 << 3,  // Timer 0
    1 << 4,  // Timer 1
    1 << 5,  // Timer 2
    1 << 8,  // DMA 0
    1 << 9,  // DMA 1
    1 << 10, // DMA 2
    1 << 11, // DMA 3
    1 << 12, // Key
    1 << 13, // Game Pak
};

#define AGB_IRQ_SLOTS \
    ((int)(sizeof(agb_irq_flag_for_slot) / sizeof(agb_irq_flag_for_slot[0])))

static volatile uint16_t *io16(int offset)
{
    return (volatile uint16_t *)(agb_mem.io + offset);
}

void agb_irq_raise(uint16_t flag)
{
    // The request latches whether or not it is serviced.
    *io16(REG_OFF_IF) |= flag;

    if (!(*io16(REG_OFF_IME) & 1))
        return;
    if (!(*io16(REG_OFF_IE) & flag))
        return;

    for (int slot = 0; slot < AGB_IRQ_SLOTS; slot++)
    {
        if (agb_irq_flag_for_slot[slot] != flag)
            continue;
        if (gIntrTable[slot])
            gIntrTable[slot]();
        break;
    }

    // The BIOS vector acknowledges after the handler returns.
    *io16(REG_OFF_IF) = flag;
}
