#!/usr/bin/env python3
"""Give a file-scope static external linkage, so the linker can replace it.

The linker can only redirect a reference that names a symbol. A static's
references do not: the compiler resolves them to `.rodata.sFoo + offset` at
compile time, and no --defsym, rename or objcopy touches them afterwards. Drop
the `static` and the same reference becomes `R_386_GOTOFF sFoo`, which a
--defsym binding overrides like any other -- and --gc-sections then drops the
section nothing points at any more.

This is the whole edit: one keyword. Compare what it replaces --
patch_data_definitions.point_at_cart had to parse the type, match braces,
preserve inner array dimensions, substitute or refuse `sizeof`, and special-case
anonymous structs and a storage class written after the struct body. None of
that applies here, because the declaration is left exactly as it stands: the
compiler still knows the type and the bounds, so `sizeof` and ARRAY_COUNT keep
working on their own.

Named symbols only, and a name that is not found is an error: a silently skipped
one would ship the data it was meant to remove.
"""
import re
import sys


def destatic(text, symbol):
    """Remove `static` from the definition of one file-scope symbol."""
    # The declaration runs from `static` to the symbol's name, with anything in
    # between: qualifiers, a type name, a whole anonymous struct body. It ends at
    # the name followed by dimensions or an initialiser, so a mention of the
    # symbol elsewhere cannot match.
    pattern = re.compile(
        r"(?:^|(?<=[;}]))([^\S\n]*)static([^\S\n]+(?:[^;{}]|\{[^{}]*\})*?"
        + re.escape(symbol) + r"[^\S\n]*(?:\[[^\]]*\])*[^\S\n]*=)",
        re.MULTILINE | re.DOTALL)
    patched, count = pattern.subn(r"\1\2", text, count=1)
    if count:
        return patched

    # text.c writes the storage class after the struct body:
    # `struct { ... } static const sKeypadIcons[] = {`.
    trailing = re.compile(
        r"(\}[^\S\n]*)static([^\S\n]+[^;{}]*?" + re.escape(symbol)
        + r"[^\S\n]*(?:\[[^\]]*\])*[^\S\n]*=)")
    patched, count = trailing.subn(r"\1\2", text, count=1)
    return patched if count else None


def rename(text, old, new):
    """Rename every mention of a file-scope static.

    A static is only visible in its own translation unit, so renaming it there
    is complete -- there is no reference anywhere else to miss. That is what
    makes the handful of names defined in several files tractable: one --defsym
    cannot point `sWindowTemplates` at seven addresses, but seven uniquely
    named symbols need no such trick.
    """
    return re.sub(r"\b" + re.escape(old) + r"\b", new, text)


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: destatic.py FILE SYMBOL[=NEWNAME] [SYMBOL...]")
    path, symbols = sys.argv[1], sys.argv[2:]
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()

    missed = []
    for symbol in symbols:
        symbol, _, newname = symbol.partition("=")
        out = destatic(text, symbol)
        if out is None:
            missed.append(symbol)
            continue
        text = rename(out, symbol, newname) if newname else out

    if missed:
        for symbol in missed:
            print(f"destatic: no static definition of {symbol} in {path}",
                  file=sys.stderr)
        return 1

    with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
