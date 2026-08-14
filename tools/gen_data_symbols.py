#!/usr/bin/env python3
"""Work out which data definitions have to come out of which file.

ADR 0006 says the binary ships no game data. Two mechanisms carry that out --
whole files that hold nothing but data are not compiled, and definitions that
share a file with code are rewritten into declarations -- and both need a list
of what to act on.

Written down by hand, that list goes stale the moment the submodule pin moves:
a symbol that gained a definition upstream would quietly start being compiled in
again, and nothing would say so. So it is derived instead, from the ROM build's
own symbol table and the decompilation's own sources.

usage: gen_data_symbols.py ROM.elf ROM.sym SRCDIR -o out.cmake [--skip sym,...]
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import elfsections


def rom_data_symbols(elf, sym):
    """Every global symbol the ROM holds outside its executable sections."""
    code = elfsections.code_ranges(elfsections.read_sections(elf))
    out = {}
    for line in open(sym):
        parts = line.split()
        if len(parts) != 4 or parts[1] != "g":
            continue
        address = int(parts[0], 16)
        if not (0x08000000 <= address < 0x09000000):
            continue
        if elfsections.is_code(code, address):
            continue
        out[parts[3]] = int(parts[2], 16)
    return out


# A definition, as opposed to a declaration or a use: a name, optional array
# bounds, then an initialiser. Kept in step with tools/patch_data_definitions.py,
# which has to find the same thing in the preprocessed copy.
DEFINITION = re.compile(
    r"^[^\S\n]*((?:[A-Za-z_][A-Za-z0-9_]*[^\S\n]+|\*+[^\S\n]*)+)"
    r"([A-Za-z_][A-Za-z0-9_]*)[^\S\n]*((?:\[[^\]]*\])*)[^\S\n]*=",
    re.MULTILINE)

INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)


def scan(srcdir, also=frozenset()):
    """symbol -> file that defines it, and file -> files it includes."""
    defines, includes = {}, {}
    for path in sorted(Path(srcdir).rglob("*")):
        if path.suffix not in (".c", ".h"):
            continue
        text = path.read_text(errors="ignore")
        rel = str(path.relative_to(srcdir))
        includes[rel] = INCLUDE.findall(text)
        for match in DEFINITION.finditer(text):
            kind, name, dims = match.group(1), match.group(2), match.group(3)

            # A static is not a symbol anything else can bind to, and prefixing
            # `extern` to one is not even valid C.
            if "static" in kind.split():
                continue

            # `gFoo[] = {...}` has no bound to carry into a declaration, so the
            # type is incomplete and anything taking ARRAY_COUNT of it stops
            # compiling. Excluded by default; --also names the ones that have
            # been built and played through, since most are only ever indexed.
            if dims.replace(" ", "") == "[]" and name not in also:
                continue

            defines.setdefault(name, rel)
    return defines, includes


def owning_units(includes):
    """header -> the .c that compiles it, computed once for every file.

    Walking the include graph per symbol is the obvious way to write this and it
    does not finish: there are twenty-three thousand symbols and a thousand
    files. One pass outwards from each .c is the same answer in a second.
    """
    owner = {}
    for unit in sorted(k for k in includes if k.endswith(".c")):
        seen, stack = set(), [unit]
        while stack:
            current = stack.pop()
            if current in seen:
                continue
            seen.add(current)
            owner.setdefault(current, unit)
            for name in includes.get(current, ()):
                if name in includes:
                    stack.append(name)
    return owner


def defined_outside_cart(binary):
    """Symbols the port defines itself rather than reading from the cart image.

    The only honest test that a symbol is still compiled in: a definition wins
    over a cart binding silently, so neither the link nor the symbol count says
    anything, but the address does.
    """
    import subprocess

    out = subprocess.run(["nm", binary], capture_output=True, text=True).stdout
    where = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) == 3 and parts[1] not in "Uw":
            where[parts[2]] = int(parts[0], 16)

    cart = where.get("agb_cart")
    if cart is None:
        sys.exit("gen_data_symbols: no agb_cart in " + binary)
    return {s for s, a in where.items() if not (cart <= a < cart + 0x1000000)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("sym")
    ap.add_argument("srcdir")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--skip", default="",
                    help="symbols to leave compiled in, comma separated")
    ap.add_argument("--skip-file", default="",
                    help="translation units to leave alone, comma separated")
    ap.add_argument("--also", default="",
                    help="unbounded arrays to include anyway, comma separated")
    ap.add_argument("--defined-in",
                    help="a linked port binary; without it every symbol the "
                         "decompilation defines is listed, and most are already "
                         "handled. With it, only the ones still compiled in.")
    args = ap.parse_args()

    skip = {s for s in args.skip.split(",") if s}
    also = {s for s in args.also.split(",") if s}
    skip_files = {s for s in args.skip_file.split(",") if s}
    leaking = defined_outside_cart(args.defined_in) if args.defined_in else None
    rom = rom_data_symbols(args.elf, args.sym)
    defines, includes = scan(args.srcdir, also)
    owner = owning_units(includes)

    by_unit, unresolved = {}, []
    for symbol in sorted(rom):
        if symbol in skip or symbol not in defines:
            continue
        if leaking is not None and symbol not in leaking:
            continue
        unit = owner.get(defines[symbol])
        if unit is None:
            unresolved.append(symbol)
            continue
        if Path(unit).name in skip_files:
            continue
        by_unit.setdefault(Path(unit).name, []).append(symbol)

    with open(args.output, "w") as fh:
        fh.write("# Generated by tools/gen_data_symbols.py. Do not edit.\n")
        fh.write("#\n# Data the game defines in C, which the player's ROM supplies instead.\n")
        fh.write("set(FRLG_GAME_DATA_SYMBOLS\n")
        for unit in sorted(by_unit):
            fh.write(f'    "{unit}={",".join(sorted(by_unit[unit]))}"\n')
        fh.write(")\n")

    total = sum(len(v) for v in by_unit.values())
    print(f"gen_data_symbols: {total} symbols across {len(by_unit)} files"
          + (f", {len(unresolved)} unplaced" if unresolved else ""))


if __name__ == "__main__":
    main()
