// The wireless adapter, enough of it to be absent safely.
//
// Link play is phase 10 and the adapter's library is not built, so every one of
// its entry points is a stub that names itself and returns zero. That is fine
// for calls, and not fine for the five pointers `rfu_initializeAPI` hands out:
// the link manager reads through them on the very next frame, and a null read is
// a segfault here where it is a harmless BIOS read on the machine.
//
// It is reached by walking into a Pokémon Center. The union-room listener starts
// there, which switches the link layer from cable to wireless, and from then on
// `HandleLinkConnection` runs the manager every frame -- so this is not a corner
// of the game, it is the first building a player enters.
//
// librfu carves its structures out of a buffer the caller supplies. Carving the
// same buffer the same way, and leaving it zeroed, is what the game sees when no
// adapter answers: a link status that reports neither parent nor child. The
// hardware half -- the serial interrupt handler and the request queue -- is what
// phase 10 adds, and none of it is needed to say "nobody is there".

#include <string.h>

#include "gba/types.h"
#include "librfu.h"

// The offsets librfu uses, kept as it writes them: each is the one before it
// plus that structure's size.
#define OFFSET_LINK_STATUS 0x000
#define OFFSET_STATIC 0x0B4
#define OFFSET_FIXED 0x0DC
#define OFFSET_SLOT_NI 0x1BC
#define OFFSET_SLOT_UNI 0x37C

u16 rfu_initializeAPI(u32 *APIBuffer, u16 buffByteSize, IntrFunc *sioIntrTable_p,
                      bool8 copyInterruptToRam)
{
    u8 *buffer = (u8 *)APIBuffer;

    (void)sioIntrTable_p;

    if (buffer == NULL || ((uintptr_t)buffer & 3) != 0)
        return ERR_RFU_API_BUFF_ADR;
    if (buffByteSize < (copyInterruptToRam ? RFU_API_BUFF_SIZE_RAM : RFU_API_BUFF_SIZE_ROM))
        return ERR_RFU_API_BUFF_SIZE;

    memset(buffer, 0, buffByteSize);

    gRfuLinkStatus = (struct RfuLinkStatus *)(buffer + OFFSET_LINK_STATUS);
    gRfuStatic = (struct RfuStatic *)(buffer + OFFSET_STATIC);
    gRfuFixed = (struct RfuFixed *)(buffer + OFFSET_FIXED);
    gRfuSlotStatusNI[0] = (struct RfuSlotStatusNI *)(buffer + OFFSET_SLOT_NI);
    gRfuSlotStatusUNI[0] = (struct RfuSlotStatusUNI *)(buffer + OFFSET_SLOT_UNI);

    for (u16 i = 1; i < RFU_CHILD_MAX; i++)
    {
        gRfuSlotStatusNI[i] = &gRfuSlotStatusNI[i - 1][1];
        gRfuSlotStatusUNI[i] = &gRfuSlotStatusUNI[i - 1][1];
    }

    // Neither parent nor child, which is the state the library leaves this in
    // until an adapter answers. Zero would read as MODE_CHILD.
    gRfuLinkStatus->parentChild = MODE_NEUTRAL;

    return 0;
}
