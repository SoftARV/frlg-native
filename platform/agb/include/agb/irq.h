#ifndef GUARD_AGB_IRQ_H
#define GUARD_AGB_IRQ_H

#include <stdint.h>

#define AGB_IRQ_VBLANK (1 << 0)
#define AGB_IRQ_HBLANK (1 << 1)
#define AGB_IRQ_VCOUNT (1 << 2)

// Dispatch one interrupt through the game's own handler table, honouring
// REG_IME and REG_IE exactly as the BIOS vector would.
void agb_irq_raise(uint16_t flag);

#endif // GUARD_AGB_IRQ_H
