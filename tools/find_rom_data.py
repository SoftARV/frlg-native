#!/usr/bin/env python3
"""List every data symbol in the binary whose bytes are in the ROM.

Symbol by symbol, not window by window. The window version this replaces asked
"does any 64-byte run of our data appear in the cartridge", which cannot see a
table shorter than its window: it reported nothing left while 183 small symbols
were still there, including item descriptions. Asking of each symbol instead has
no such floor, and names what to extract rather than only whether to worry.

A short symbol can match by chance -- four bytes of zeros appear everywhere --
so a minimum size applies, and symbols whose contents are too uniform to be
distinctive are reported separately rather than silently dropped.

usage: find_rom_data.py [BINARY] [ROM]
"""
import subprocess
import sys

MIN_BYTES = 8
MIN_DISTINCT = 3

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/rom-play/ports/desktop/frlg-native"
ROM = sys.argv[2] if len(sys.argv) > 2 else "vendor/pokefirered/pokefirered.gba"

DATA_SECTIONS = (".rodata", ".data.rel.ro", ".data")


def main():
    rom = open(ROM, "rb").read()
    blob = open(BIN, "rb").read()

    secs = {}
    for line in subprocess.run(["readelf", "-S", "-W", BIN],
                               capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) > 6 and p[0].startswith("["):
            try:
                i = 2 if p[1] == "]" else 1
                name, addr = p[i], int(p[i + 2], 16)
                off, size = int(p[i + 3], 16), int(p[i + 4], 16)
                if addr and name in DATA_SECTIONS:
                    secs[name] = (addr, off, size)
            except (ValueError, IndexError):
                pass

    # Sized symbols that live in a data section.
    syms = []
    for line in subprocess.run(["nm", "-S", "--defined-only", BIN],
                               capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) == 4 and p[2] in "RrDd":
            try:
                syms.append((int(p[0], 16), int(p[1], 16), p[3]))
            except ValueError:
                pass

    found, uniform, checked = [], [], 0
    for addr, size, name in syms:
        if size < MIN_BYTES:
            continue
        for sec, (base, off, length) in secs.items():
            if base <= addr < base + length:
                at = off + (addr - base)
                data = blob[at:at + size]
                if len(data) != size:
                    break
                checked += 1
                if len(set(data)) < MIN_DISTINCT:
                    uniform.append((name, size))
                elif rom.find(data) >= 0:
                    found.append((name, size))
                break

    print(f"checked {checked} data symbols of at least {MIN_BYTES} bytes")
    if uniform:
        print(f"{len(uniform)} were too uniform to judge and were skipped")
    if not found:
        print("\nNo data symbol's contents appear in the ROM.")
        return 0
    total = sum(n for _, n in found)
    print(f"\n{len(found)} symbols carry ROM data, {total} bytes in all:\n")
    for name, size in sorted(found, key=lambda x: -x[1]):
        print(f"  {size:7d}  {name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
