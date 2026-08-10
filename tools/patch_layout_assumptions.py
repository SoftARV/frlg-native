#!/usr/bin/env python3
"""Remove upstream's reliance on where the linker put two variables.

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
without assuming anything about where they are. Both are matched exactly and
both must be found, so a submodule bump is reported rather than silently
reintroducing the corruption.
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


REPLACEMENTS = [
    (
        "ClearSav2",
        _combined_fill("gSaveBlock2", "gSaveBlock2_DMA"),
        "{ vu16 tmp = (vu16)(0);"
        " CpuSet((void *)&tmp, &gSaveBlock2, 0x01000000 | ((sizeof(struct SaveBlock2))/2 & 0x1FFFFF));"
        " CpuSet((void *)&tmp, gSaveBlock2_DMA, 0x01000000 | ((sizeof(gSaveBlock2_DMA))/2 & 0x1FFFFF)); }",
    ),
    (
        "ClearSav1",
        _combined_fill("gSaveBlock1", "gSaveBlock1_DMA"),
        "{ vu16 tmp = (vu16)(0);"
        " CpuSet((void *)&tmp, &gSaveBlock1, 0x01000000 | ((sizeof(struct SaveBlock1))/2 & 0x1FFFFF));"
        " CpuSet((void *)&tmp, gSaveBlock1_DMA, 0x01000000 | ((sizeof(gSaveBlock1_DMA))/2 & 0x1FFFFF)); }",
    ),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    args = ap.parse_args()

    text = open(args.file).read()

    for what, old, new in REPLACEMENTS:
        found = text.count(old)
        if found != 1:
            sys.exit(
                f"patch_layout_assumptions: expected exactly one occurrence of the "
                f"{what} fill in {args.file}, found {found}. Upstream has changed it; "
                "see docs/ARCHITECTURE.md 4.3."
            )
        text = text.replace(old, new, 1)

    open(args.file, "w").write(text)


if __name__ == "__main__":
    main()
