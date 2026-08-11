#!/usr/bin/env python3
"""Replace the statements that reach hardware no host has.

Three files are upstream C that would compile but for one statement each of
hardware -- and a fourth spins on one. Rather than override a whole file for a
line, the line is replaced in the preprocessed copy:

  m4a.c     swi 0x2A     asks the BIOS to fill the sequencer's dispatch table.
                         Nothing in this game calls the function holding it --
                         the table is filled by MPlayJumpTableCopy, which we
                         supply, so the call simply goes.

  m4a.c     VCOUNT wait  SampleFreqSet spins until the display reaches
                         scanline 159 to phase-align timer 0. A whole frame is
                         rendered inside one signal handler here, so the game
                         thread is suspended for the entire sweep and reads 0
                         whenever it resumes: the wait can never end. Nothing is
                         lost, because the host reads the mixer's PCM buffer
                         directly rather than through timer 0.

  main.c    VBlank spin  the main loop's wait for the V-blank flag. It stays a
                         spin in a real-time run, where a signal preempts it
                         (ADR 0009), and the body is empty. In lockstep it is the
                         one point the game is provably idle, so the frame is
                         advanced from inside it and a run stops depending on
                         wall-clock time at all -- see ADR 0013. Calling out of
                         the loop is what makes that possible.

  script.c  svc 2        BIOS Halt, inside a loop that never ends, on finding a
                         corrupt script pointer. The game means to stop there,
                         and waiting for the next V-blank for ever does the same
                         without the BIOS. This one line was excluding the whole
                         script VM, and with it forty-eight routines the game
                         calls while showing a message.

Every edit is matched exactly and must be found. A submodule bump that moves or
rewrites one fails the build rather than silently changing what is compiled. A
blanket erasure of `asm` would be wrong: global.h rewrites it to `__asm__`, and
m4a.c's preprocessed copy carries a second one -- glibc's asm label on
strerror_r.

See docs/ARCHITECTURE.md 4.3.
"""

import argparse
import os
import re
import sys

# Keyed by the source file each belongs to. A replacement of "" removes the
# statement; anything else stands in for it.
EDITS = {
    "m4a.c": [
        (
            "the BIOS dispatch-table call",
            re.compile(r'[ \t]*__asm__\("swi 0x2A"\);\n'),
            "",
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
            "",
        ),
    ],
    "main.c": [
        (
            "the main loop's V-blank spin",
            re.compile(
                r"while \(!\(gMain\.intrCheck & \(1 << 0\)\)\)\n[ \t]*;\n"
            ),
            # Spelled out rather than captured: replacements here are taken
            # literally, so a group reference would land as one.
            "while (!(gMain.intrCheck & (1 << 0)))\n        agb_frame_idle();\n",
        ),
    ],
    "script.c": [
        (
            "the script VM's halt",
            re.compile(r'__asm__\("svc 2"\);'),
            # BIOS Halt waits for an interrupt, and the loop around it never
            # ends: the game is telling us a script pointer is corrupt and it
            # intends to stop here. Waiting for the next V-blank forever does the
            # same thing without the BIOS, and without spinning a core.
            "VBlankIntrWait();",
        ),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    args = ap.parse_args()

    edits = EDITS.get(os.path.basename(args.file))
    if edits is None:
        sys.exit(f"strip_hardware_waits: nothing known about {args.file}")

    text = open(args.file).read()

    for what, pattern, replacement in edits:
        found = len(pattern.findall(text))
        if found != 1:
            sys.exit(
                f"strip_hardware_waits: expected exactly one occurrence of {what} "
                f"in {args.file}, found {found}. Upstream has changed it; see "
                "docs/ARCHITECTURE.md 4.3."
            )
        text = pattern.sub(replacement.replace("\\", "\\\\"), text, count=1)

    open(args.file, "w").write(text)


if __name__ == "__main__":
    main()
