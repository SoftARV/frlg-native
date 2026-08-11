#!/usr/bin/env python3
"""Repair reads that only a machine without memory protection tolerates.

The Game Boy Advance has no MMU. Every address in its map is readable, address
zero included -- that region is BIOS ROM -- so a read through a null pointer
returns garbage rather than killing the program. Upstream code is entitled to
that, and in at least one place it relies on it.

`naming_screen.c` frees its own state inside `RunTasks()`, and `CB2_NamingScreen`
then calls `AnimateSprites()` in the same frame. The cursor's callback reads
`sNamingScreen->currentPage` through the pointer that was just set to null. On
hardware that reads BIOS ROM at offset 0x1e22, the cursor jumps once, the main
callback changes and it never runs again -- invisible. Here it is a segfault, and
it is what stopped a player naming their character.

Freeing without clearing the pointer leaves the read hitting freed heap instead
of address zero: still garbage, still read exactly once, and still overwritten by
the next allocation, but mapped. Nothing tests the pointer for null except the
check immediately after the allocation that sets it, so nothing depends on it
being cleared.

The alternative -- mapping a readable page at address zero so the whole class
behaves as it does on hardware -- is not available: `vm.mmap_min_addr` forbids it
without privileges we should not want.

Matched exactly and required to be found, so a submodule bump is reported rather
than quietly restoring the crash. See docs/ARCHITECTURE.md 4.3.
"""

import argparse
import os
import re
import sys

# Matched after preprocessing, where FREE_AND_SET_NULL has expanded into a block
# whose halves are separated by line directives -- hence a pattern rather than a
# string. The free stays; only the assignment goes.
EDITS = {
    "naming_screen.c": [
        (
            "the naming screen's read through its freed state",
            re.compile(
                r"\{ Free\(sNamingScreen\); sNamingScreen =\s*"
                r"(?:#[^\n]*\n\s*)*\(\(void \*\)0\)\s*"
                r"(?:#[^\n]*\n\s*)*; \}"
            ),
            "{ Free(sNamingScreen); }",
        ),
    ],
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    args = ap.parse_args()

    edits = EDITS.get(os.path.basename(args.file))
    if edits is None:
        sys.exit(f"patch_null_tolerance: nothing known about {args.file}")

    text = open(args.file).read()

    for what, pattern, new in edits:
        found = len(pattern.findall(text))
        if found != 1:
            sys.exit(
                f"patch_null_tolerance: expected exactly one occurrence of {what} "
                f"in {args.file}, found {found}. Upstream has changed it; see "
                "docs/ARCHITECTURE.md 4.3."
            )
        text = pattern.sub(new, text, count=1)

    open(args.file, "w").write(text)


if __name__ == "__main__":
    main()
