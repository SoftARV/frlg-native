#!/usr/bin/env python3
"""Check that every extracted symbol's cartridge bytes are the bytes we expect.

Extraction replaces a definition with a pointer into the ROM. That is only
correct if the ROM holds what the definition held -- and the two are produced by
different compilers, so it is not a given. `struct BgTemplate` is four bytes in
both, and agbcc packs its bitfields across the two storage units where ours
starts a new one: same size, different bits, backgrounds that never appear.

Size comparisons cannot see that. This can: the build that compiles the data in
holds what the source says, the cartridge holds what agbcc made of it, and the
two are compared byte for byte at the address the extraction binds.

A symbol only in the extracted build is skipped -- there is nothing to compare
it against -- and so is one whose sizes differ, which the size audit already
covers.

usage: verify_extraction.py [COMPILED_IN_BINARY] [LIST] [ROM]
"""
import re
import subprocess
import sys

BIN = sys.argv[1] if len(sys.argv) > 1 else "build/retail-play/ports/desktop/frlg-native"
LIST = sys.argv[2] if len(sys.argv) > 2 else "cmake/game_data_symbols_statics.cmake"
ROM = sys.argv[3] if len(sys.argv) > 3 else "vendor/pokefirered/pokefirered.gba"

CART_BASE = 0x08000000


def relocated_words(path=None):
    """Cart offsets holding a pointer the importer rewrites.

    Those four bytes cannot match: ours hold an address from this link, the
    cartridge holds one from the ROM's. Comparing them says nothing, so they
    are masked out of both sides.
    """
    import os as _os
    import re as _re
    if path is None:
        here = _os.path.dirname(_os.path.abspath(__file__))
        path = _os.path.join(here, "..", "build", "rom-play", "ports", "desktop",
                             "agb_relocations.c")
    try:
        text = open(path).read()
    except OSError:
        return set()
    out = set()
    for m in _re.finditer(r"0x([0-9a-fA-F]{8})", text):
        out.add(int(m.group(1), 16))
    return out


def extracted(path):
    """symbol -> {cart offsets}, since a name may be defined in several files."""
    out = {}
    for line in open(path):
        m = re.search(r'"([^"=]+)=([^"]*)"', line)
        if not m:
            continue
        for s in m.group(2).split(","):
            if "#" in s:
                name, _, rest = s.partition("#")
                out.setdefault(name, set()).add(int(rest.split("@")[0], 16))
    return out


def main():
    rom = open(ROM, "rb").read()
    blob = open(BIN, "rb").read()
    want = extracted(LIST)

    secs = []
    for line in subprocess.run(["readelf", "-S", "-W", BIN],
                               capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) > 6 and p[0].startswith("["):
            try:
                i = 2 if p[1] == "]" else 1
                addr, off, size = int(p[i + 2], 16), int(p[i + 3], 16), int(p[i + 4], 16)
                if addr and p[i] in (".rodata", ".data.rel.ro", ".data"):
                    secs.append((addr, off, size))
            except (ValueError, IndexError):
                pass

    def at(vaddr):
        for a, o, n in secs:
            if a <= vaddr < a + n:
                return o + (vaddr - a)
        return None

    relocs = relocated_words()
    print(f"masking {len(relocs)} relocated words", file=sys.stderr)
    checked, bad, skipped = 0, [], 0
    for line in subprocess.run(["nm", "-S", "--defined-only", BIN],
                               capture_output=True, text=True).stdout.splitlines():
        p = line.split()
        if len(p) != 4 or p[3] not in want or p[2] not in "RrDd":
            continue
        vaddr, size, name = int(p[0], 16), int(p[1], 16), p[3]
        pos = at(vaddr)
        if pos is None or size == 0:
            skipped += 1
            continue
        ours = blob[pos:pos + size]
        if len(ours) != size:
            skipped += 1
            continue
        checked += 1
        # A name defined in several files has several candidates; matching any
        # of them means this copy is bound somewhere correct.
        matched, first_diff = False, None
        for off in want[name]:
            theirs = rom[off:off + size]
            if len(theirs) != size:
                continue
            diff = [i for i in range(size)
                    if ours[i] != theirs[i] and (off + (i & ~3)) not in relocs]
            if not diff:
                matched = True
                break
            if first_diff is None:
                first_diff = diff[0]
        if not matched:
            bad.append((name, size, first_diff if first_diff is not None else 0))

    print(f"compared {checked} extracted symbols against the cartridge "
          f"({skipped} skipped)")
    if not bad:
        print("\nEvery one holds the bytes the source compiles to.")
        return 0
    print(f"\n{len(bad)} differ -- the cartridge's copy is not what the code expects:\n")
    for name, size, first in sorted(bad, key=lambda x: -x[1]):
        print(f"  {size:6d} bytes, first difference at {first:4d}  {name}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
