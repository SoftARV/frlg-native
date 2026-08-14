#!/usr/bin/env python3
"""Give union AffineAnimCmd the width the cartridge gives it.

agbcc rounds every structure size up to a multiple of four, so a cartridge's
affine animation commands sit eight bytes apart. Our compilers make the union
six -- correctly, by their own ABI -- and reading a cartridge table with a
six-byte stride misparses every command after the first. The animation then
never reaches its END marker: it never completes, the matrix is never written,
and a sprite keeps whatever pixels were already in its buffer. union AnimCmd
needs nothing, being four bytes in both worlds already.

The definition is rewritten in the preprocessed copy because vendor's header is
pinned and never edited. A file that names the type but no longer matches the
shape below is an error rather than a silent revert to the narrow layout, so a
submodule bump is reported.
"""
import re
import sys

# Types whose data this port reads out of the cart. Confirmed against the ROM's
# own symbol sizes: gMonFrontPicCoords is 0x6e0 over 440 entries, four bytes
# each, where this build would give two. Types that only ever live in RAM are
# deliberately absent -- their layout is this build's business, and widening
# them would change the save format.
TYPES = (
    ("union", "AffineAnimCmd", 8),
    ("struct", "MonCoords", 4),
)

ALIGNED = "__attribute__((aligned(4)))"


def widen(text, kw, name, width, path):
    """Give one type the cartridge's width, and lock it there."""
    definition = re.compile(rf"{kw}\s+{name}\s*\{{(?P<body>[^{{}}]*?)\}}\s*;", re.S)
    marker = f"sizeof({kw} {name})"

    if f"{kw} {name}" not in text:
        return text, False
    if marker in text:
        return text, False   # already widened; the stage is idempotent

    patched, count = definition.subn(
        lambda m: "%s %s {%s} %s;\n_Static_assert(%s == %d,\n"
                  "    \"%s must match the cartridge's %d-byte layout\");"
                  % (kw, name, m.group("body"), ALIGNED, marker, width, name, width),
        text, count=1)
    if count == 0:
        sys.exit(f"patch_struct_layout: {path} names {kw} {name} but its definition "
                 "does not match the expected shape; the pin moved and the layout "
                 "fix must be rechecked")
    return patched, True


def main():
    if len(sys.argv) != 2:
        sys.exit("usage: patch_struct_layout.py file.c")
    path = sys.argv[1]
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()

    changed = False
    for kw, name, width in TYPES:
        text, did = widen(text, kw, name, width, path)
        changed = changed or did

    if changed:
        with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
            fh.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
