#!/usr/bin/env python3
"""Which extracted symbols have a type the cartridge lays out differently?

Every symbol the port reads out of the cart is looked up in the decompilation's
sources to find its element type, and that type is checked against the sizes
this build computes. A symbol whose type is not a multiple of four bytes wide
here is misparsed from its second element onward.
"""
import re
import subprocess
import sys

ROOT = "/home/miguel/Developer/fireRecomp/frlg-native"
VENDOR = f"{ROOT}/vendor/pokefirered"
TMP = "/home/miguel/.claude/jobs/e10e2825/tmp"

# --- our sizes for every type, from a preprocessed TU -------------------------
PP = f"{ROOT}/build/rom-play/platform/agb/pp/pokemon.c"
text = open(PP, encoding="utf-8", errors="surrogateescape").read()
types = []
for kw in ("struct", "union"):
    for m in re.finditer(rf"^{kw}\s+(\w+)\s*\{{", text, re.M):
        types.append((kw, m.group(1)))
seen = set()
types = [t for t in types if not (t in seen or seen.add(t))]

probe = [text, "\n"]
for kw, name in types:
    probe.append(f"char __p_{kw}_{name}[sizeof({kw} {name})];\n")
open(f"{TMP}/mt.c", "w", encoding="utf-8", errors="surrogateescape").write("".join(probe))
r = subprocess.run(["gcc", "-m32", "-std=gnu11", "-w", "-c", f"{TMP}/mt.c",
                    "-o", f"{TMP}/mt.o"], capture_output=True, text=True)
if r.returncode != 0:
    sys.exit(r.stderr[-1500:])
size = {}
for line in subprocess.run(["nm", "-S", f"{TMP}/mt.o"], capture_output=True,
                           text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 4 and p[3].startswith("__p_"):
        kw, name = p[3][len("__p_"):].split("_", 1)
        size[name] = int(p[1], 16)

# --- the symbols the port extracts -------------------------------------------
wanted = set()
for line in open(f"{ROOT}/cmake/game_data_symbols.cmake"):
    m = re.search(r'"([^"=]+)=([^"]*)"', line)
    if m:
        for s in m.group(2).split(","):
            if s:
                wanted.add(s.split("@")[0].split("#")[0])
# data-only files contribute every symbol they define
for f in ("data.c", "graphics.c", "tilesets.c", "strings.c"):
    o = f"{VENDOR}/build/firered/src/{f[:-2]}.o"
    out = subprocess.run(["arm-none-eabi-nm", "--defined-only", o],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] in "RrDd":
            wanted.add(p[2])

# --- what type is each one? ---------------------------------------------------
decl = re.compile(
    r"(?:^|\n)\s*(?:extern\s+)?(?:const\s+)?(struct|union)\s+(\w+)\s+"
    r"(?:\*\s*)?(?:const\s+)?(\w+)\s*\[")
sources = subprocess.run(["grep", "-rlE", r"(struct|union) \w+ \w+\[", f"{VENDOR}/src",
                          f"{VENDOR}/include"], capture_output=True, text=True).stdout.split()
sym_type = {}
for path in sources:
    try:
        body = open(path, encoding="utf-8", errors="surrogateescape").read()
    except OSError:
        continue
    for kw, tname, sym in decl.findall(body):
        if sym in wanted and sym not in sym_type:
            sym_type[sym] = (kw, tname)

bad = {}
for sym, (kw, tname) in sym_type.items():
    n = size.get(tname)
    if n is not None and n % 4 != 0:
        bad.setdefault((kw, tname, n, (n + 3) // 4 * 4), []).append(sym)

print(f"{len(wanted)} extracted symbols; {len(sym_type)} resolved to a struct/union type")
print(f"{len(bad)} of those types are laid out differently by the cartridge:\n")
for (kw, tname, ours, theirs), syms in sorted(bad.items(), key=lambda x: -len(x[1])):
    print(f"  {kw} {tname:32s} {ours:3d} -> {theirs:3d}   {len(syms)} symbol(s)")
    for s in sorted(syms)[:6]:
        print(f"      {s}")
    if len(syms) > 6:
        print(f"      ... and {len(syms)-6} more")
