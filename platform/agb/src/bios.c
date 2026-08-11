// GBA BIOS calls, replacing libagbsyscall.s.
//
// The arithmetic entries must reproduce the BIOS bit for bit, including its
// rounding, because game logic consumes the values. They are unit-tested
// against hardware-derived vectors in phase 3; until then treat the affine
// and ArcTan2 results as unverified.

#include <string.h>

#include "gba/types.h"
#include "gba/syscall.h"

#include "agb/memmap.h"

#define CPU_SET_SRC_FIXED_BIT 0x01000000
#define CPU_SET_32BIT_BIT 0x04000000
#define CPU_SET_COUNT_MASK 0x001FFFFF

void CpuSet(const void *src, void *dest, u32 control)
{
    u32 count = control & CPU_SET_COUNT_MASK;
    u32 unit = (control & CPU_SET_32BIT_BIT) ? 4 : 2;
    u8 *out = dest;

    if (control & CPU_SET_SRC_FIXED_BIT)
    {
        for (u32 i = 0; i < count; i++, out += unit)
            memcpy(out, src, unit);
    }
    else
    {
        memmove(out, src, (size_t)count * unit);
    }
}

void CpuFastSet(const void *src, void *dest, u32 control)
{
    // Always 32-bit, and the hardware moves whole 8-word blocks.
    u32 count = ((control & CPU_SET_COUNT_MASK) + 7) & ~7u;
    u8 *out = dest;

    if (control & CPU_SET_SRC_FIXED_BIT)
    {
        for (u32 i = 0; i < count; i++, out += 4)
            memcpy(out, src, 4);
    }
    else
    {
        memmove(out, src, (size_t)count * 4);
    }
}

void RegisterRamReset(u32 resetFlags)
{
    if (resetFlags & RESET_EWRAM)
        memset(agb_mem.ewram, 0, sizeof(agb_mem.ewram));
    // IWRAM's top 0x200 bytes hold the interrupt vector and stacks on hardware.
    if (resetFlags & RESET_IWRAM)
        memset(agb_mem.iwram, 0, sizeof(agb_mem.iwram) - 0x200);
    if (resetFlags & RESET_PALETTE)
        memset(agb_mem.pltt, 0, sizeof(agb_mem.pltt));
    if (resetFlags & RESET_VRAM)
        memset(agb_mem.vram, 0, sizeof(agb_mem.vram));
    if (resetFlags & RESET_OAM)
        memset(agb_mem.oam, 0, sizeof(agb_mem.oam));
    if (resetFlags & RESET_SIO_REGS)
        memset(agb_mem.io + 0x120, 0, 0x10);
    if (resetFlags & RESET_SOUND_REGS)
        memset(agb_mem.io + 0x60, 0, 0x50);
    if (resetFlags & RESET_REGS)
    {
        memset(agb_mem.io, 0, 0x60);
        // The clear covers both affine blocks, and the game does not put them
        // back: AgbMain resets every register on the way in and the trainer
        // pictures are still drawn with the identity a thousand frames later.
        // The reference emulator's renderer holds the identity there across the
        // same call, so the register clear cannot be leaving them at zero.
        agb_io_affine_identity();
    }
}

s32 Div(s32 num, s32 denom)
{
    if (denom == 0)
        return 0;
    return num / denom;
}

u16 Sqrt(u32 num)
{
    u32 root = 0;
    u32 rem = 0;

    for (int i = 16; i > 0; i--)
    {
        rem = (rem << 2) | (num >> 30);
        num <<= 2;
        root <<= 1;
        if (rem > root)
        {
            rem -= root | 1;
            root += 2;
        }
    }
    return (u16)(root >> 1);
}

u16 ArcTan2(s16 x, s16 y)
{
    // The BIOS returns a 16-bit binary angle: 0x0000 is +x, 0x4000 is +y.
    if (y == 0)
        return x < 0 ? 0x8000 : 0x0000;
    if (x == 0)
        return y < 0 ? 0xC000 : 0x4000;

    s32 ax = x < 0 ? -x : x;
    s32 ay = y < 0 ? -y : y;
    u32 oct;

    if (ay < ax)
        oct = (u32)(((s64)ay << 14) / ax);
    else
        oct = 0x4000 - (u32)(((s64)ax << 14) / ay);

    // Small-angle tangent correction, matching the BIOS table's shape.
    oct = (oct * (0x4000 - ((oct * oct) >> 16) / 6)) >> 14;

    if (x >= 0)
        return (u16)(y >= 0 ? oct : (u32)(-(s32)oct));
    return (u16)(y >= 0 ? 0x8000 - oct : 0x8000 + oct);
}

void BgAffineSet(struct BgAffineSrcData *src, struct BgAffineDstData *dest, s32 count)
{
    extern s16 agb_sin_lut(u16 angle);

    for (s32 i = 0; i < count; i++, src++, dest++)
    {
        s32 sx = src->sx;
        s32 sy = src->sy;
        s16 sinv = agb_sin_lut(src->alpha);
        s16 cosv = agb_sin_lut((u16)(src->alpha + 0x4000));

        dest->pa = (s16)((sx * cosv) >> 14);
        dest->pb = (s16)((-sx * sinv) >> 14);
        dest->pc = (s16)((sy * sinv) >> 14);
        dest->pd = (s16)((sy * cosv) >> 14);

        dest->dx = src->texX - (dest->pa * src->scrX + dest->pb * src->scrY);
        dest->dy = src->texY - (dest->pc * src->scrX + dest->pd * src->scrY);
    }
}

void ObjAffineSet(struct ObjAffineSrcData *src, void *dest, s32 count, s32 offset)
{
    extern s16 agb_sin_lut(u16 angle);
    s16 *out = dest;

    for (s32 i = 0; i < count; i++, src++)
    {
        s32 sx = src->xScale;
        s32 sy = src->yScale;
        s16 sinv = agb_sin_lut(src->rotation);
        s16 cosv = agb_sin_lut((u16)(src->rotation + 0x4000));

        *out = (s16)((sx * cosv) >> 14);
        out += offset / 2;
        *out = (s16)((-sx * sinv) >> 14);
        out += offset / 2;
        *out = (s16)((sy * sinv) >> 14);
        out += offset / 2;
        *out = (s16)((sy * cosv) >> 14);
        out += offset / 2;
    }
}

static void lz77_decompress(const void *src, void *dest)
{
    const u8 *in = src;
    u8 *out = dest;
    u32 header = (u32)in[0] | ((u32)in[1] << 8) | ((u32)in[2] << 16) | ((u32)in[3] << 24);
    u32 remaining = header >> 8;

    in += 4;
    while (remaining > 0)
    {
        u8 flags = *in++;

        for (int bit = 0; bit < 8 && remaining > 0; bit++)
        {
            if (flags & 0x80)
            {
                u32 len = (in[0] >> 4) + 3;
                u32 disp = (((u32)(in[0] & 0xF) << 8) | in[1]) + 1;
                in += 2;

                if (len > remaining)
                    len = remaining;
                for (u32 i = 0; i < len; i++, out++)
                    *out = *(out - disp);
                remaining -= len;
            }
            else
            {
                *out++ = *in++;
                remaining--;
            }
            flags <<= 1;
        }
    }
}

void LZ77UnCompWram(const void *src, void *dest)
{
    lz77_decompress(src, dest);
}

void LZ77UnCompVram(const void *src, void *dest)
{
    // VRAM rejects byte writes on hardware; our arena does not care.
    lz77_decompress(src, dest);
}

void SoftReset(u32 resetFlags)
{
    extern void agb_soft_reset(u32 flags);
    agb_soft_reset(resetFlags);
}

void VBlankIntrWait(void)
{
    extern void agb_wait_vblank(void);
    agb_wait_vblank();
}
