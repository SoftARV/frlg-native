#!/usr/bin/env python3
"""Work out where each named symbol lives in the ROM, using the linker map.

Matching a symbol to its ROM address by size works until several files define
the same name at the same size -- `sWindowTemplates` is 32 bytes in seven of
them -- and then it cannot tell which is which. The map can: it records where
each object's section was placed, and `nm` on that object gives the symbol's
offset inside it. The sum is the address, with no ambiguity to resolve.

Emits `file.c=name#offset@size` lines for the extraction list. Statics get an
offset; globals are named without one, since the linker binds those itself.

usage: resolve_by_map.py NAMES_FILE [MAP] [OBJDIR]
"""
import collections
import glob
import re
import subprocess
import sys

CART_BASE = 0x08000000

names_file = sys.argv[1]
MAP = sys.argv[2] if len(sys.argv) > 2 else "vendor/pokefirered/pokefirered.map"
OBJDIR = sys.argv[3] if len(sys.argv) > 3 else "vendor/pokefirered/build/firered/src"

wanted = {l.split()[0] for l in open(names_file) if l.strip()}
wanted.discard("gCrc16Table")

# Where each object's sections were placed.
placement = collections.defaultdict(dict)
line_re = re.compile(r"^\s*(\.\w[\w.]*)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+\.o)\s*$")
for line in open(MAP, errors="replace"):
    m = line_re.match(line)
    if m:
        section, addr, _, obj = m.groups()
        placement[obj.split("/")[-1]][section] = int(addr, 16)

entries = collections.defaultdict(list)
unresolved = []
for path in sorted(glob.glob(f"{OBJDIR}/*.o")):
    obj = path.split("/")[-1]
    base = obj[:-2] + ".c"
    out = subprocess.run(["arm-none-eabi-nm", "-S", "--defined-only", path],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) != 4 or p[3] not in wanted or p[2] not in "RrDdBb":
            continue
        offset, size, kind, name = int(p[0], 16), int(p[1], 16), p[2], p[3]
        # Which section is it in? nm does not say, so try the ones a constant
        # can live in, in the order the map lists them.
        for section in (".rodata", ".data", ".bss"):
            at = placement.get(obj, {}).get(section)
            if at is None:
                continue
            addr = at + offset
            if not (CART_BASE <= addr < CART_BASE + 0x2000000):
                continue
            if kind.islower():
                entries[base].append(f"{name}#{addr - CART_BASE:#x}@{size}")
            else:
                entries[base].append(f"{name}@{size}")
            break
        else:
            unresolved.append((base, name))

found = {e.split("#")[0].split("@")[0] for v in entries.values() for e in v}
print(f"resolved {sum(len(v) for v in entries.values())} definitions of "
      f"{len(found)} names across {len(entries)} files", file=sys.stderr)
for name in sorted(wanted - found):
    print(f"  not found in any object: {name}", file=sys.stderr)
for base, name in unresolved[:10]:
    print(f"  no section placement: {base} {name}", file=sys.stderr)

for base in sorted(entries):
    print(f'{base}={",".join(sorted(set(entries[base])))}')
