#!/usr/bin/env python3
"""Generate the cart relocation table.

The ROM's data holds pointers, and they are the addresses the original build
gave its own code and RAM. Loading that image into the cart region does not make
them mean anything here, so each one is rewritten once, at load.

What each record needs depends on where it points, and two of the classes need
nothing at all -- see docs/spikes/0005-relocation-classes.md:

  data           the target is ROM data with no name to link against: shift the
                 pointer into the cart
  global code    the target is a function: use ours of that name
  global RAM     the target is a variable: use ours, plus the addend
  global data    the target is named ROM data: use ours of that name too. For
                 data only the ROM has, that resolves into the cart anyway, since
                 those symbols are bound there at link time. For data our build
                 compiles, it is the difference between the game reading our copy
                 and reading the cart's -- and the cart's is raw ROM bytes, whose
                 own pointers were never rewritten.
  interior       the target is inside a ROM function -- a jump table the
                 original compiler emitted. No native counterpart exists and
                 nothing reads it. Left alone.
  local          the target is a static, so it has no name to link against.
                 Every one sits in ROM data our own build re-creates, and with
                 named data resolved above, the game reads our copy rather than
                 the cart's -- which is what makes leaving these alone safe.
  16-bit         not a pointer at all but a script constant. Left alone.

The output is C rather than data because the code and RAM targets are named
symbols: writing them as `&symbol` lets the linker resolve them, so nothing has
to look symbols up at runtime.
"""

import argparse
import re
import struct
import subprocess
import sys

ROM_BASE = 0x08000000
DATA_SECTIONS = {".rel.data", ".rel.rodata", ".relscript_data"}

# Symbol names that are really section names: a relocation against one of these
# carries an offset rather than naming what it points at.
SECTION_SYMBOLS = {".text", ".data", ".rodata", "script_data", "ewram", "iwram"}


def read_sections(elf, readelf):
    """Section name -> (address, size), for classifying targets."""
    out = subprocess.run([readelf, "-SW", elf], capture_output=True, text=True, check=True).stdout
    sections = {}
    for line in out.splitlines():
        m = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+\S+\s+([0-9a-f]{8})\s+[0-9a-f]+\s+([0-9a-f]+)", line)
        if m:
            sections[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16))
    return sections


def read_global_symbols(elf, readelf):
    """Names with external linkage. A static has no name we can link against."""
    out = subprocess.run([readelf, "-sW", elf], capture_output=True, text=True, check=True).stdout
    names = set()
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 8 and f[0].endswith(":") and f[4] in ("GLOBAL", "WEAK"):
            names.add(f[7])
    return names


def read_relocations(elf, readelf):
    out = subprocess.run([readelf, "-rW", elf], capture_output=True, text=True, check=True).stdout
    section = None
    for line in out.splitlines():
        m = re.match(r"Relocation section '(\S+)'", line)
        if m:
            section = m.group(1)
            continue
        if section not in DATA_SECTIONS:
            continue
        f = line.split()
        if len(f) < 4 or not re.fullmatch(r"[0-9a-f]{8}", f[0]):
            continue
        yield int(f[0], 16), f[2], int(f[3], 16), (f[4] if len(f) > 4 else "")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("rom")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--readelf", default="arm-none-eabi-readelf")
    ap.add_argument("--report", action="store_true")
    args = ap.parse_args()

    rom = open(args.rom, "rb").read()
    sections = read_sections(args.elf, args.readelf)
    if ".text" not in sections:
        sys.exit("gen_relocations: no .text section; is this the linked ELF?")
    text_start, text_size = sections[".text"]
    text_end = text_start + text_size
    rom_end = ROM_BASE + len(rom)

    ram_ranges = []
    for name in ("ewram", "iwram"):
        if name in sections:
            base, size = sections[name]
            ram_ranges.append((base, base + size))

    global_names = read_global_symbols(args.elf, args.readelf)
    if not global_names:
        sys.exit("gen_relocations: no global symbols; is this the linked ELF?")

    data_offsets = []
    symbol_records = []   # (offset, symbol, addend)
    interior = local = wrong_width = 0

    for offset, kind, symval, name in read_relocations(args.elf, args.readelf):
        site = offset - ROM_BASE
        if not (0 <= site <= len(rom) - 4):
            sys.exit(f"gen_relocations: relocation at {offset:#x} falls outside the ROM")

        if kind != "R_ARM_ABS32":
            # A narrower slot cannot hold a native address, and nothing that
            # needs relocating was stored in one. Counted so the total adds up.
            wrong_width += 1
            continue

        word = struct.unpack_from("<I", rom, site)[0]

        if any(lo <= word < hi for lo, hi in ram_ranges):
            if name in SECTION_SYMBOLS:
                interior += 1
                continue
            if name not in global_names:
                local += 1
                continue
            symbol_records.append((site, name, word - symval))
        elif text_start <= (word & ~1) < text_end:
            if name in SECTION_SYMBOLS:
                interior += 1
                continue
            if name not in global_names:
                local += 1
                continue
            # Thumb function pointers carry bit 0; native addresses do not.
            symbol_records.append((site, name, 0))
        elif text_end <= word < rom_end:
            # ROM data. If the target has a name, use ours of that name rather
            # than the copy in the cart -- and it comes to the same thing for
            # data only the ROM has, because those symbols are bound into the
            # cart region at link time (ADR 0006). It does not come to the same
            # thing for data our build compiles: the cart's copy of it is raw ROM
            # bytes, so any pointer *inside* it is still a GBA address. Shifting
            # a pointer to such a table hands the game that stale copy, and it
            # jumps through whatever the table holds.
            if name in SECTION_SYMBOLS or name not in global_names:
                data_offsets.append(site)
            else:
                symbol_records.append((site, name, word - symval))
        else:
            sys.exit(f"gen_relocations: {offset:#x} stores {word:#010x}, "
                     "which is in no region this knows about")

    symbols = sorted({name for _, name, _ in symbol_records})

    with open(args.output, "w") as fh:
        fh.write("// Generated by tools/gen_relocations.py. Do not edit.\n")
        fh.write("//\n// See docs/spikes/0005-relocation-classes.md for what each class means.\n\n")
        fh.write("#include <stdint.h>\n\n#include \"agb/cart.h\"\n\n")

        fh.write("// Declared as arrays so that naming one yields its address, whether it is a\n"
                 "// function or a variable. No real declaration is in scope here, deliberately:\n"
                 "// upstream's own prototypes disagree with some of these.\n")
        for name in symbols:
            fh.write(f"extern char {name}[];\n")

        fh.write(f"\nconst uint32_t agb_reloc_data[] = {{\n")
        for i in range(0, len(data_offsets), 8):
            fh.write("    " + " ".join(f"{o:#010x}," for o in data_offsets[i:i + 8]) + "\n")
        fh.write("};\n")
        fh.write(f"const uint32_t agb_reloc_data_count = {len(data_offsets)};\n\n")

        fh.write("const struct agb_reloc_symbol agb_reloc_symbols[] = {\n")
        for site, name, addend in symbol_records:
            fh.write(f"    {{{site:#010x}, {name}, {addend}}},\n")
        fh.write("};\n")
        fh.write(f"const uint32_t agb_reloc_symbol_count = {len(symbol_records)};\n")

    if args.report:
        total = len(data_offsets) + len(symbol_records) + interior + local + wrong_width
        print(f"relocations: {total}")
        print(f"  data            {len(data_offsets):6d}")
        print(f"  global symbols  {len(symbol_records):6d}  ({len(symbols)} distinct)")
        print(f"  interior        {interior:6d}  left alone")
        print(f"  local targets   {local:6d}  left alone")
        print(f"  not 32-bit      {wrong_width:6d}  left alone")


if __name__ == "__main__":
    main()
