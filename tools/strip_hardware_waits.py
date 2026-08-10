#!/usr/bin/env python3
"""Remove the two statements in m4a.c that no host can run.

The sequencer is upstream C and needs no override. Two statements in it are
hardware, though, and neither survives being compiled for a host:

  swi 0x2A       asks the BIOS to fill the sequencer's dispatch table. Nothing
                 in this game calls the function holding it -- the table is
                 filled by MPlayJumpTableCopy, which we supply.

  the VCOUNT     SampleFreqSet spins until the display reaches scanline 159, to
  wait           phase-align timer 0. A whole frame is rendered inside one
                 signal handler here, so the game thread is suspended for the
                 entire sweep and reads 0 whenever it resumes; the wait can
                 never end. We do not drive audio from timer 0 either -- the
                 host reads the mixer's PCM buffer directly -- so what the wait
                 is for does not apply.

Both are matched exactly and both must be found. A submodule bump that moves or
rewrites either one fails the build rather than silently changing what is
compiled. A blanket erasure would be wrong for the first: global.h rewrites
`asm` to `__asm__`, and the preprocessed file carries a second one -- glibc's
asm label on strerror_r.

See docs/ARCHITECTURE.md 6.7 and docs/ROADMAP.md phase 4.
"""

import argparse
import re
import sys

REMOVALS = [
    (
        "the BIOS dispatch-table call",
        re.compile(r'[ \t]*__asm__\("swi 0x2A"\);\n'),
    ),
    (
        "the VCOUNT scanline wait",
        re.compile(
            r"[ \t]*while \(\*\(vu8 \*\)\(\(\(uintptr_t\)agb_mem\.io\) \+ 0x6\) == 159\)\n"
            r"[ \t]*;\n"
            r"\s*"
            r"[ \t]*while \(\*\(vu8 \*\)\(\(\(uintptr_t\)agb_mem\.io\) \+ 0x6\) != 159\)\n"
            r"[ \t]*;\n"
        ),
    ),
]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    args = ap.parse_args()

    text = open(args.file).read()

    for what, pattern in REMOVALS:
        found = len(pattern.findall(text))
        if found != 1:
            sys.exit(
                f"strip_hardware_waits: expected exactly one occurrence of {what} "
                f"in {args.file}, found {found}. Upstream has changed it; see "
                "docs/ARCHITECTURE.md 6.7."
            )
        text = pattern.sub("", text, count=1)

    open(args.file, "w").write(text)


if __name__ == "__main__":
    main()
