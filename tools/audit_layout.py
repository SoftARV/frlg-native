#!/usr/bin/env python3
"""Find extracted symbols whose type the cartridge lays out differently.

agbcc pads some structures our compilers do not, so a type whose size here is
not a multiple of four is a candidate for occupying fewer bytes here than in the
ROM. Every table of such a type read out of the cart is then misparsed from its
second element onward -- the first is correct, which is what makes the class so
quiet ([ADR 0018](../docs/adr/0018-match-the-cartridges-structure-layout.md)).

Sizes come from the binary's own debug information, so every type the port
compiles is covered rather than the ones a single translation unit happens to
include. Membership is never assumed from the size alone: a candidate is only
real once its ROM symbol size divides by the entry count to something wider than
ours, which is why the report prints the arithmetic instead of a verdict.

usage: audit_layout.py [BINARY] [SYMBOL_LIST]
"""
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = sys.argv[1] if len(sys.argv) > 1 else f"{ROOT}/build/rom-play/ports/desktop/frlg-native"
LIST = sys.argv[2] if len(sys.argv) > 2 else f"{ROOT}/cmake/game_data_symbols.cmake"
VENDOR = f"{ROOT}/vendor/pokefirered"


def type_sizes(binary):
    """Every struct/union size this build uses, from DWARF."""
    out = subprocess.run(["pahole", "--sizes", binary], capture_output=True, text=True)
    sizes = {}
    for line in out.stdout.splitlines():
        parts = line.split("\t")
        if len(parts) >= 2:
            try:
                sizes[parts[0]] = int(parts[1])
            except ValueError:
                pass
    return sizes


def extracted_symbols(path):
    """Every symbol the cart supplies: the per-symbol list, plus whole files.

    Files in FRLG_GAME_DATA_ONLY are cut wholesale and never appear in the
    symbol list, so reading only that list misses everything they define --
    which is how the trainer party structs reached a battle unaudited.
    """
    names = set()
    for line in open(path):
        m = re.search(r'"([^"=]+)=([^"]*)"', line)
        if m:
            for s in m.group(2).split(","):
                if s:
                    names.add(s.split("@")[0].split("#")[0])

    pipeline = open(f"{ROOT}/cmake/GamePipeline.cmake").read()
    block = re.search(r"set\(FRLG_GAME_DATA_ONLY\n(.*?)\)\n", pipeline, re.S)
    for base in (block.group(1).split() if block else []):
        obj = f"{VENDOR}/build/firered/src/{base[:-2]}.o"
        if not os.path.exists(obj):
            continue
        out = subprocess.run(["arm-none-eabi-nm", "--defined-only", obj],
                             capture_output=True, text=True).stdout
        for line in out.splitlines():
            p = line.split()
            if len(p) == 3 and p[1] in "RrDdBb":
                names.add(p[2])
    return names


def symbols_of_type(names):
    """Map a symbol to the struct/union it is an array of, from the sources."""
    decl = re.compile(
        r"(?:^|\n)\s*(?:extern\s+)?(?:static\s+)?(?:const\s+)?(struct|union)\s+(\w+)\s+"
        r"(?:\*\s*)?(?:const\s+)?(\w+)\s*\[")
    found = {}
    for base in (f"{VENDOR}/src", f"{VENDOR}/include"):
        for dirpath, _, files in os.walk(base):
            for name in files:
                if not name.endswith((".c", ".h")):
                    continue
                path = os.path.join(dirpath, name)
                try:
                    body = open(path, encoding="utf-8", errors="surrogateescape").read()
                except OSError:
                    continue
                for kw, tname, sym in decl.findall(body):
                    if sym in names and sym not in found:
                        found[sym] = (kw, tname)
    return found


def main():
    sizes = type_sizes(BINARY)
    if not sizes:
        sys.exit("audit_layout: pahole returned nothing; is the binary built with debug info?")
    names = extracted_symbols(LIST)
    owned = symbols_of_type(names)

    suspect = {}
    for sym, (kw, tname) in owned.items():
        n = sizes.get(tname)
        if n is not None and n % 4 != 0:
            suspect.setdefault((kw, tname, n, (n + 3) // 4 * 4), []).append(sym)

    print(f"{len(sizes)} types in {os.path.basename(BINARY)}; "
          f"{len(names)} extracted symbols, {len(owned)} resolved to a struct or union")
    if not suspect:
        print("\nNo extracted symbol uses a type whose size is not a multiple of four.")
        return
    print(f"\n{len(suspect)} type(s) need checking against the ROM's own symbol sizes:\n")
    for (kw, tname, ours, theirs), syms in sorted(suspect.items(), key=lambda x: -len(x[1])):
        print(f"  {kw} {tname:34s} ours {ours:3d}  agbcc would pad to {theirs:3d}")
        for s in sorted(syms)[:8]:
            print(f"      {s}")
        if len(syms) > 8:
            print(f"      ... and {len(syms) - 8} more")
    print("\nConfirm each against the ROM before widening: the symbol's size in the\n"
          "reference build divided by its entry count in the source is the cartridge's\n"
          "stride. struct LevelUpMove is two bytes in both worlds and must not be widened.")


if __name__ == "__main__":
    main()
