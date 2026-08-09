#ifndef GUARD_AGB_PRELUDE_H
#define GUARD_AGB_PRELUDE_H

// Force-included ahead of every game translation unit. Pulling upstream's
// hardware headers in here sets their include guards, so the game's own
// `#include "gba/..."` lines short-circuit and the overrides below survive.
// Path-order shadowing cannot do this: a quoted include resolves against the
// including file's own directory first, which is always vendor/include.
//
// Quoted, not angled: vendor/include must stay off the bracket chain, or the
// game's strings.h hijacks POSIX <strings.h> for every host header.
#include "gba/types.h"
#include "gba/defines.h"
#include "gba/io_reg.h"
#include "gba/macro.h"

#include "memmap.h"
#include "dma.h"

// Derived macros (EWRAM_END, BG_PLTT, OBJ_VRAM0, REG_ADDR_*, ...) expand at
// point of use, so redefining the bases redirects everything built on them.
#undef EWRAM_START
#undef IWRAM_START
#undef PLTT
#undef VRAM
#undef OAM
#undef REG_BASE

#define EWRAM_START ((uintptr_t)agb_mem.ewram)
#define IWRAM_START ((uintptr_t)agb_mem.iwram)
#define PLTT ((uintptr_t)agb_mem.pltt)
#define VRAM ((uintptr_t)agb_mem.vram)
#define OAM ((uintptr_t)agb_mem.oam)
#define REG_BASE ((uintptr_t)agb_mem.io)

#undef SOUND_INFO_PTR
#undef INTR_CHECK
#undef INTR_VECTOR

#define SOUND_INFO_PTR (*(struct SoundInfo **)(IWRAM_START + 0x7FF0))
#define INTR_CHECK (*(u16 *)(IWRAM_START + 0x7FF8))
#define INTR_VECTOR (*(void **)(IWRAM_START + 0x7FFC))

// The host linker places these wherever it likes; only their contents matter.
#undef IWRAM_DATA
#undef EWRAM_DATA
#undef COMMON_DATA
#define IWRAM_DATA
#define EWRAM_DATA
#define COMMON_DATA

// Save flash. The real driver executes Thumb code copied into a stack buffer,
// so it is replaced wholesale (docs/ARCHITECTURE.md 6.8); this keeps every other
// reader of FLASH_BASE pointed at real memory.
#include "gba/flash_internal.h"
#undef FLASH_BASE
#define FLASH_BASE ((u8 *)agb_mem.sram)

// Arming a DMA channel has an immediate side effect, so it cannot stay a plain
// register write. DmaCopy/DmaFill/DmaClear all expand through DmaSet.
#undef DmaSet
#define DmaSet(dmaNum, src, dest, control) \
    agb_dma_set((dmaNum), (const void *)(src), (void *)(dest), (uint32_t)(control))

#undef DmaStop
#define DmaStop(dmaNum) agb_dma_stop(dmaNum)

#endif // GUARD_AGB_PRELUDE_H
