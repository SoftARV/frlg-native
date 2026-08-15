#!/usr/bin/env python3
"""List every symbol in the binary whose bytes also appear in the ROM.

The random-chunk probe answers "is there game data in here" with a sample, which
is fine for a trend and useless for finishing: extract what it found and it finds
something else next run. This walks the binary's read-only data instead, window
by window, and names every symbol whose contents are present in the cartridge.
That turns "essentially none" into a list with an end.

A match is not proof of copying on its own -- a run of zeros or a short common
sequence appears anywhere -- so windows are only reported when they carry enough
distinct byte values to be data rather than filler.

usage: find_rom_data.py [BINARY] [ROM]
"""
import subprocess
import sys

WINDOW = 64
STRIDE = 32
MIN_DISTINCT = 12

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/rom-play/ports/desktop/frlg-native"
ROM = sys.argv[2] if len(sys.argv) > 2 else "vendor/pokefirered/pokefirered.gba"


def sections(binary):
    out = subprocess.run(["readelf", "-S", "-W", binary], capture_output=True, text=True).stdout
    found = []
    for line in out.splitlines():
        p = line.split()
        if len(p) > 6 and p[0].startswith("["):
            try:
                i = 2 if p[1] == "]" else 1
                name, addr = p[i], int(p[i + 2], 16)
                off, size = int(p[i + 3], 16), int(p[i + 4], 16)
                if addr and name in (".rodata", ".data.rel.ro", ".data"):
                    found.append((name, off, size, addr))
            except (ValueError, IndexError):
                pass
    return found


def symbols(binary):
    out = subprocess.run(["nm", "-n", "--defined-only", binary],
                         capture_output=True, text=True).stdout
    got = []
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3:
            try:
                got.append((int(p[0], 16), p[2]))
            except ValueError:
                pass
    return got


def main():
    rom = open(ROM, "rb").read()
    blob = open(BIN, "rb").read()
    syms = symbols(BIN)

    def owner(vaddr):
        prev = None
        for a, s in syms:
            if a > vaddr:
                break
            prev = (a, s)
        return prev[1] if prev else "?"

    hits, scanned = {}, 0
    for name, off, size, addr in sections(BIN):
        for at in range(off, off + size - WINDOW, STRIDE):
            w = blob[at:at + WINDOW]
            if len(set(w)) < MIN_DISTINCT:
                continue
            scanned += 1
            if rom.find(w) >= 0:
                sym = owner(addr + (at - off))
                hits[sym] = hits.get(sym, 0) + 1

    print(f"scanned {scanned} data windows of {WINDOW} bytes")
    if not hits:
        print("\nNo window of the binary's data is present in the ROM.")
        return
    total = sum(hits.values())
    print(f"\n{len(hits)} symbols carry ROM data, {total} windows in all:\n")
    for sym, n in sorted(hits.items(), key=lambda x: -x[1]):
        print(f"  {n:5d}  {sym}")


if __name__ == "__main__":
    main()
