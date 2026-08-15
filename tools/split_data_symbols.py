#!/usr/bin/env python3
"""Split the extraction list into the two ways of removing a definition.

A **global** is cut from the source and left as a declaration; the linker then
binds the unresolved name into the cart. A **static** is not cut at all -- it
only loses the `static` keyword, and a --defsym binding overrides its definition
so --gc-sections drops the section. See ADR 0020.

A handful of statics share a name across files (`sWindowTemplates` in seven of
them). Giving those external linkage would collide at link time, and one
--defsym cannot point the same name at several addresses, so they keep the old
source rewrite until they are renamed per object.

Emits, for one source file, the arguments each tool should be given, and -- once
per build -- the response file of --defsym bindings for every destaticed symbol.

usage: split_data_symbols.py LIST FILE.c {--cut|--destatic}
       split_data_symbols.py LIST --defsyms OUT.rsp
"""
import re
import sys

# Names defined in more than one translation unit. Kept as a list rather than
# discovered, so that a new collision is a build error rather than a silent
# change of mechanism.
SHARED_NAMES = {
    "sAnim_Cloud",
    "sBGTemplates",
    "sBgTemplates",
    "sBg_Gfx",
    "sBg_Pal",
    "sBg_Tilemap",
    "sMartMaps",
    "sOamData_ItemIcon",
    "sOamData_Star",
    "sStar_Gfx",
    "sStar_Pal",
    "sTextColorTable",
    "sTextColors",
    "sTiles",
    "sWindowTemplate",
    "sWindowTemplates",
    "sWindowTemplates_Results",
    "sYesNoWindowTemplate",
}


def unique(base, name):
    """A name for a static that several files define, one per file."""
    return "frlg_" + base[:-2].replace("-", "_") + "_" + name


def read(path):
    """file -> [(name, offset_or_None, size_or_None)]"""
    out = {}
    for line in open(path):
        m = re.search(r'"([^"=]+)=([^"]*)"', line)
        if not m:
            continue
        entries = []
        for s in m.group(2).split(","):
            if not s:
                continue
            name, _, rest = s.partition("#")
            if rest:
                off, _, size = rest.partition("@")
                entries.append((name, off, size))
            else:
                name, _, size = s.partition("@")
                entries.append((name, None, size))
        out[m.group(1)] = entries
    return out


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__.strip().splitlines()[-2])
    if sys.argv[1] != "--hoisted":
        listing = read(sys.argv[1])

    if sys.argv[1] == "--hoisted":
        # `file.c=name=newname#offset` entries, appended to the response file.
        with open(sys.argv[2], "a") as fh:
            for entry in sys.argv[3:]:
                _, _, rest = entry.partition("=")
                _, _, rest = rest.partition("=")
                newname, _, off = rest.partition("#")
                fh.write(f"-Wl,--defsym,{newname}=agb_cart+{off}\n")
        return 0

    if sys.argv[2] == "--defsyms":
        lines = []
        for base, entries in listing.items():
            for name, off, _ in entries:
                if off:
                    bound = unique(base, name) if name in SHARED_NAMES else name
                    lines.append(f"-Wl,--defsym,{bound}=agb_cart+{off}\n")
        with open(sys.argv[3], "w") as fh:
            fh.writelines(sorted(lines))
        print(f"static bindings: {len(lines)}", file=sys.stderr)
        return 0

    base, mode = sys.argv[2], sys.argv[3]
    names = []
    for name, off, size in listing.get(base, []):
        static = off is not None
        if mode == "--destatic" and static:
            names.append(f"{name}={unique(base, name)}" if name in SHARED_NAMES
                         else name)
        elif mode == "--cut" and not static:
            names.append(name + (f"#{off}" if off else "") + (f"@{size}" if size else ""))
    print(";".join(names), end="")
    return 0


if __name__ == "__main__":
    sys.exit(main())
