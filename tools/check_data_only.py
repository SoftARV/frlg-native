#!/usr/bin/env python3
"""Verify that the files we refuse to compile really are only data.

FRLG_GAME_DATA_ONLY names translation units whose symbols are bound into the
cart region instead of being compiled. That is only safe while they contain no
code: a file that grows a function would have it silently dropped, and the
symbol would then bind to whatever the ROM has at that address -- ARM machine
code, which this port cannot execute.

Checked against the ROM build's own objects, since those are what upstream
compiled and this port does not build these files at all.
"""

import subprocess
import sys
from pathlib import Path


def text_size(readelf, obj):
    out = subprocess.run([readelf, "-SW", str(obj)], capture_output=True, text=True).stdout
    total = 0
    for line in out.splitlines():
        parts = line.replace("]", "] ").split()
        # [ nr] name type addr off size ...
        if len(parts) > 6 and parts[2].startswith(".text") and parts[3] == "PROGBITS":
            total += int(parts[6], 16)
    return total


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: check_data_only.py READELF OBJDIR file.c [file.c...]")
    readelf, objdir, names = sys.argv[1], Path(sys.argv[2]), sys.argv[3:]

    missing, offending = [], []
    for name in names:
        obj = objdir / (name[:-2] + ".o")
        if not obj.exists():
            missing.append(name)
            continue
        size = text_size(readelf, obj)
        if size:
            offending.append(f"{name}: {size} bytes of code")

    if missing:
        print("check_data_only: no object for " + ", ".join(missing), file=sys.stderr)
    if offending:
        print("check_data_only: these are not data-only any more:", file=sys.stderr)
        for line in offending:
            print("  " + line, file=sys.stderr)
        sys.exit(1)
    print(f"check_data_only: {len(names) - len(missing)} files carry no code")


if __name__ == "__main__":
    main()
