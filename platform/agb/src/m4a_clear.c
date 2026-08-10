// The sequencer's 64-byte clear.
//
// Its own file because upstream's header declares it `void SoundMainBTM(void)`
// while it in fact receives a pointer: it is only ever reached through the
// dispatch table, whose entries are unprototyped, so nothing upstream ever
// noticed. Defining it correctly means not seeing that declaration.
//
// The name is the original SDK's and describes nothing -- m4a.c's Clear64byte
// dispatches to it as gMPlayJumpTable[35]. It has no part in mixing.

#include <stdint.h>

void SoundMainBTM(void *dest)
{
    uint32_t *p = dest;

    for (int i = 0; i < 16; i++)
        p[i] = 0;
}
