#!/usr/bin/env python3
"""Remove upstream's reliance on where the linker put its save blocks.

Two sites in `load_save.c`, both writing past the end of an object because the
cartridge's linker script guarantees what follows it.

`ClearSav1` and `ClearSav2` each clear one object by asking for the size of
*two*:

    CpuFill16(0, &gSaveBlock2, sizeof(struct SaveBlock2) + sizeof(gSaveBlock2_DMA));

On the cartridge that is correct, because `ld_script.ld` places `gSaveBlock2`
and `gSaveBlock2_DMA` next to each other, so one fill covers both. Nothing
places them next to each other here -- they are ordinary globals and the host
linker orders them as it likes. When it puts the `_DMA` array *first*, the fill
runs off the end of the save block and into whatever follows.

It followed into the music players in the optimised build, which is how this was
found: the sequencer was writing silence because its player structs were being
zeroed a few frames after startup, while the debug build happened to lay the
two symbols out adjacently and worked by luck. See
docs/spikes/0006-release-build-silence.md.

Each call becomes two, one per object, which clears exactly the same bytes
without assuming anything about where they are.

The second site is `SetSaveBlocksPointers`, which shifts all three save blocks by
a random offset of up to `SAVEBLOCK_MOVE_RANGE` bytes:

    offset = (Random()) & ((SAVEBLOCK_MOVE_RANGE - 1) & ~3);
    gSaveBlock2Ptr = (void *)(&gSaveBlock2) + offset;

and then writes whole structs through those pointers. The cartridge's linker
script leaves that much headroom after each block; nothing here does, so the tail
of every save block lands in whichever global the host linker put next. The
offset is forced to zero -- which is one of the values upstream itself picks --
while leaving the `Random()` call in place, because removing it would shift every
later draw and change what the game does.

That one cost an evening twice over: the same music players, zeroed the same way,
by a different line in the same file. See docs/spikes/0006-release-build-silence.md.

Every edit is matched exactly and must be found, so a submodule bump is reported
rather than silently reintroducing the corruption.
"""

import argparse
import sys

# Matched after preprocessing, where CpuFill16 has already expanded, so these are
# the expanded forms. Splitting the size expression is enough: the fill covers one
# object, and a second identical fill covers the other.
def _combined_fill(block, dma):
    """The one call upstream makes: one destination, the size of two objects."""
    return ("{ vu16 tmp = (vu16)(0); CpuSet((void *)&tmp, &%s, 0x00000000 | 0x01000000 "
            "| ((sizeof(struct %s) + sizeof(%s))/(16/8) & 0x1FFFFF)); }"
            % (block, block.removeprefix("g"), dma))


LOAD_SAVE = [
    (
        "ClearSav2",
        _combined_fill("gSaveBlock2", "gSaveBlock2_DMA"),
        "{ vu16 tmp = (vu16)(0);"
        " CpuSet((void *)&tmp, &gSaveBlock2, 0x01000000 | ((sizeof(struct SaveBlock2))/2 & 0x1FFFFF));"
        " CpuSet((void *)&tmp, gSaveBlock2_DMA, 0x01000000 | ((sizeof(gSaveBlock2_DMA))/2 & 0x1FFFFF)); }",
    ),
    (
        "save block shuffle",
        "offset = (Random()) & ((128 - 1) & ~3);",
        # The call stays: it draws from the same RNG every later roll comes from,
        # and dropping it would shift the whole sequence.
        "offset = (Random()) & 0;",
    ),
    (
        "ClearSav1",
        _combined_fill("gSaveBlock1", "gSaveBlock1_DMA"),
        "{ vu16 tmp = (vu16)(0);"
        " CpuSet((void *)&tmp, &gSaveBlock1, 0x01000000 | ((sizeof(struct SaveBlock1))/2 & 0x1FFFFF));"
        " CpuSet((void *)&tmp, gSaveBlock1_DMA, 0x01000000 | ((sizeof(gSaveBlock1_DMA))/2 & 0x1FFFFF)); }",
    ),
]


# A pointer split across two `s16` sprite data slots and put back together:
#
#     *(u16 *)(sprite->data[6] | (sprite->data[7] << 16))
#
# `data[6]` is signed, so promoting it sign-extends, and if the address's low
# half has bit 15 set every high bit turns on and the `|` cannot clear them.
# In GBA RAM those variables sit low enough that it never happens. Here
# gSpriteCoordOffsetY is at 0x086994a0 -- low half 0x94a0 -- and the read went
# to 0xFFFF94A0 and killed the game, on Metal Claw, which is the animation that
# reaches the branch selecting that particular variable.
#
# Upstream casts in battle_anim_mons.c and not here, so the fix is theirs: make
# the three read the low half unsigned, as the fourth already does.
BATTLE_ANIM_NORMAL = [
    (
        "sign-extended pointer halves",
        "*(u16 *)(sprite->data[6] | (sprite->data[7] << 16))",
        "*(u16 *)((u16)sprite->data[6] | (sprite->data[7] << 16))",
        3,
    ),
]

REPLACEMENTS = {
    "load_save.c": [(what, old, new, 1) for what, old, new in LOAD_SAVE],
    "battle_anim_normal.c": BATTLE_ANIM_NORMAL,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    args = ap.parse_args()

    text = open(args.file).read()

    import os
    base = os.path.basename(args.file)
    for what, old, new, expect in REPLACEMENTS.get(base, []):
        found = text.count(old)
        if found != expect:
            sys.exit(
                f"patch_layout_assumptions: expected {expect} occurrence(s) of the "
                f"{what} in {args.file}, found {found}. Upstream has changed it; "
                "see docs/ARCHITECTURE.md 4.3."
            )
        text = text.replace(old, new, expect)

    open(args.file, "w").write(text)


if __name__ == "__main__":
    main()
