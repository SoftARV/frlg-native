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
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import elfsections


def rom_static_data(elf, sym):
    """File-scope statics holding data, name -> cart offset.

    A static cannot be bound -- no other translation unit can name it -- so it
    is pointed at the cart image instead. A name defined in more than one file
    is skipped: the ROM's table has an entry per definition, and picking the
    wrong one would silently point the code at another file's data.
    """
    code = elfsections.code_ranges(elfsections.read_sections(elf))
    seen, out = {}, {}
    for line in open(sym):
        parts = line.split()
        if len(parts) != 4 or parts[1] != "l":
            continue
        address = int(parts[0], 16)
        if not (0x08000000 <= address < 0x09000000):
            continue
        if elfsections.is_code(code, address):
            continue
        name = parts[3]
        seen[name] = seen.get(name, 0) + 1
        out[name] = address - 0x08000000
    return {n: o for n, o in out.items() if seen[n] == 1}


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
    """symbol -> file that defines it, file -> includes, symbol -> unbounded?"""
    defines, includes, unbounded = {}, {}, {}
    for path in sorted(Path(srcdir).rglob("*")):
        if path.suffix not in (".c", ".h"):
            continue
        text = path.read_text(errors="ignore")
        rel = str(path.relative_to(srcdir))
        includes[rel] = INCLUDE.findall(text)
        for match in DEFINITION.finditer(text):
            kind, name, dims = match.group(1), match.group(2), match.group(3)

            # Statics are recorded too. They cannot be declared away -- nothing
            # can bind to the name -- but they can be pointed at the cart, and
            # they hold most of what is left. The global pass below only walks
            # global symbols, so their presence here changes nothing for it.
            # `gFoo[] = {...}` carries no bound, so the declaration works one
            # out from the size the ROM records -- see patch_data_definitions.py.
            # The symbol is tagged with that size below.
            defines.setdefault(name, rel)
            unbounded.setdefault(name, dims.replace(" ", "") == "[]")
    return defines, includes, unbounded


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
                # A quoted include resolves against the including file's own
                # directory first, and the decompilation uses that:
                # src/data/pokemon/pokedex_text.h says "pokedex_text_fr.h",
                # not the path from src/. Missing this orphaned 774 symbols --
                # every Pokedex entry's text -- which then stayed compiled in.
                beside = str(Path(current).parent / name)
                for candidate in (name, beside):
                    if candidate in includes:
                        stack.append(candidate)
                        break
    return owner


def unbindable_sites(elf, rom_path, readelf="arm-none-eabi-readelf"):
    """Offsets holding a pointer to a static function.

    Such a pointer cannot shift into the cart -- native code is not in the cart
    image -- and cannot rebind, because a static has no name another translation
    unit can reference. It keeps a cartridge address, and this port links near
    0x08000000, so it lands inside our own image and reads as plausible garbage
    rather than faulting: no crash, no link error, nothing audible, just
    something drawn or timed wrong.

    Harmless while the structure holding it is read from our copy. Fatal once it
    is read from the cart, so any symbol containing one is left compiled in.
    """
    import struct

    sections = elfsections.read_sections(elf, readelf)
    code = elfsections.code_ranges(sections)
    data_sections = elfsections.data_relocation_sections(sections)
    rom = open(rom_path, "rb").read()

    globals_ = set()
    for line in subprocess.run([readelf, "-sW", elf], capture_output=True,
                               text=True).stdout.splitlines():
        parts = line.split()
        if len(parts) >= 8 and parts[4] == "GLOBAL":
            globals_.add(parts[7])

    out, section = set(), None
    listing = subprocess.run([readelf, "-rW", elf], capture_output=True, text=True).stdout
    for line in listing.splitlines():
        match = re.match(r"Relocation section '(\S+)'", line)
        if match:
            section = match.group(1)
            continue
        if section not in data_sections:
            continue
        parts = line.split()
        if len(parts) < 5 or not re.fullmatch(r"[0-9a-f]{8}", parts[0]):
            continue

        site = int(parts[0], 16)
        if not (0x08000000 <= site < 0x08000000 + len(rom) - 4):
            continue
        word = struct.unpack_from("<I", rom, site - 0x08000000)[0]
        if not elfsections.is_code(code, word & ~1):
            continue
        if parts[4] in globals_:
            continue          # names a global: rebinds to our function, fine
        out.add(site)
    return out


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
    ap.add_argument("--statics", action="store_true",
                    help="also point file-scope statics at the cart image")
    ap.add_argument("--rom", help="the ROM image, to find pointers that cannot "
                                  "be resolved either way")
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
    statics = rom_static_data(args.elf, args.sym) if args.statics else {}
    # Where each symbol is and how big it is. rom_data_symbols returns sizes
    # alone, which is not enough to ask whether a site falls inside one.
    extent = {}
    for line in open(args.sym):
        parts = line.split()
        if len(parts) == 4:
            extent[parts[3]] = (int(parts[0], 16), int(parts[2], 16))
    defines, includes, unbounded = scan(args.srcdir, also)
    owner = owning_units(includes)

    unbindable = unbindable_sites(args.elf, args.rom) if args.rom else set()

    by_unit, unresolved, unsafe = {}, [], []
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

        # A symbol holding a pointer nothing can resolve stays compiled in.
        start, size = extent.get(symbol, (0, 0))
        if size and any(start <= site < start + size for site in unbindable):
            unsafe.append(symbol)
            continue
        # An array with no bound of its own needs the ROM's byte count to build
        # a complete type from.
        size = extent.get(symbol, (0, 0))[1]
        entry = symbol
        if unbounded.get(symbol) and size:
            entry = f"{symbol}@{size}"
        by_unit.setdefault(Path(unit).name, []).append(entry)

    # Statics come after the globals: they are pointed at the cart rather than
    # declared away, and the patcher tells them apart by the marker.
    for symbol in sorted(statics):
        if symbol in skip or symbol not in defines:
            continue
        if leaking is not None and symbol not in leaking:
            continue
        unit = owner.get(defines[symbol])
        if unit is None or Path(unit).name in skip_files:
            continue
        by_unit.setdefault(Path(unit).name, []).append(f"{symbol}#{statics[symbol]:#x}")

    with open(args.output, "w") as fh:
        fh.write("# Generated by tools/gen_data_symbols.py. Do not edit.\n")
        fh.write("#\n# Data the game defines in C, which the player's ROM supplies instead.\n")
        fh.write("set(FRLG_GAME_DATA_SYMBOLS\n")
        for unit in sorted(by_unit):
            fh.write(f'    "{unit}={",".join(sorted(by_unit[unit]))}"\n')
        fh.write(")\n")

    total = sum(len(v) for v in by_unit.values())
    print(f"gen_data_symbols: {total} symbols across {len(by_unit)} files"
          + (f", {len(unsafe)} left compiled in (unresolvable pointer)" if unsafe else "")
          + (f", {len(unresolved)} unplaced" if unresolved else ""))


if __name__ == "__main__":
    main()
