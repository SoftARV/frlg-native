#include "agb/memmap.h"

struct agb_memory agb_mem;

uint8_t agb_cart[AGB_CART_SIZE];

// PA and PD of each affine block, in the order BG2 then BG3.
#define REG_OFF_BG2PA 0x020
#define REG_OFF_BG2PD 0x026
#define REG_OFF_BG3PA 0x030
#define REG_OFF_BG3PD 0x036
// 1.0 in the affine registers' 8.8 fixed point.
#define AFFINE_ONE 0x0100

// The affine matrices are the identity rather than zero at rest, and the game
// relies on it: the trainer pictures are drawn on an affine BG2 that nothing
// ever writes a matrix for. With PA zero every pixel of a scanline samples texel
// zero, so the layer draws nothing and Oak never appears. See spike 0008.
void agb_io_affine_identity(void)
{
    static const int params[] = {REG_OFF_BG2PA, REG_OFF_BG2PD, REG_OFF_BG3PA,
                                 REG_OFF_BG3PD};

    for (unsigned i = 0; i < sizeof(params) / sizeof(params[0]); i++)
        *(volatile uint16_t *)(agb_mem.io + params[i]) = AFFINE_ONE;
}
