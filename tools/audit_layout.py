#!/usr/bin/env python3
"""List every game type agbcc lays out differently than this build does.

agbcc rounds a structure's size up to a multiple of four. Our compilers do not,
so any type whose natural size is not already a multiple of four occupies fewer
bytes here than it does in the cartridge -- and every table of that type read
out of the cart is misparsed from its second element onward.

Sizes come from a preprocessed game translation unit, which already carries
every type definition with the port's own prelude applied. Each type is probed
as an array symbol so the file need only compile, never link.
"""
import os
import re
import subprocess
import sys

PP = sys.argv[1] if len(sys.argv) > 1 else "build/rom-play/platform/agb/pp/pokemon.c"
TMP = os.environ.get("TMPDIR", "/tmp")

text = open(PP, encoding="utf-8", errors="surrogateescape").read()

# Definitions, not forward declarations: the brace must follow the name.
types = []
for kw in ("struct", "union"):
    for m in re.finditer(rf"^{kw}\s+(\w+)\s*\{{", text, re.M):
        types.append((kw, m.group(1)))
seen = set()
types = [t for t in types if not (t in seen or seen.add(t))]
print(f"{len(types)} type definitions found in {PP.split('/')[-1]}", flush=True)

probe = [text, "\n/* layout probes */\n"]
for kw, name in types:
    probe.append(f"char __probe_{kw}_{name}[sizeof({kw} {name})];\n")
open(f"{TMP}/probe.c", "w", encoding="utf-8", errors="surrogateescape").write("".join(probe))

r = subprocess.run(["gcc", "-m32", "-std=gnu11", "-w", "-c", f"{TMP}/probe.c",
                    "-o", f"{TMP}/probe.o"], capture_output=True, text=True)
if r.returncode != 0:
    print(r.stderr[-2000:])
    sys.exit("probe failed to compile")

sizes = {}
out = subprocess.run(["nm", "-S", f"{TMP}/probe.o"], capture_output=True, text=True).stdout
for line in out.splitlines():
    p = line.split()
    if len(p) == 4 and p[3].startswith("__probe_"):
        sizes[p[3][len("__probe_"):]] = int(p[1], 16)

bad = []
for kw, name in types:
    key = f"{kw}_{name}"
    if key in sizes and sizes[key] % 4 != 0:
        bad.append((kw, name, sizes[key], (sizes[key] + 3) // 4 * 4))

print(f"\n{len(bad)} types differ between this build and the cartridge:\n")
print(f"  {'type':52s} {'ours':>5s} {'agbcc':>6s}")
for kw, name, ours, theirs in sorted(bad, key=lambda x: x[1]):
    print(f"  {kw + ' ' + name:52s} {ours:5d} {theirs:6d}")
