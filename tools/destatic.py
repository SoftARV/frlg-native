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
    """Remove `static` from the definition of one file-scope symbol.

    Found by locating the definition and walking back to the keyword, rather
    than by matching forward from every `static`. The forward version had to
    describe everything that can sit between the keyword and the name -- an
    anonymous struct body among it -- and that pattern backtracks
    catastrophically on a multi-megabyte preprocessed source: forty-six of them
    ran for hours without finishing. Walking back is linear and guesses nothing,
    which also means an attribute in front of the keyword, or a storage class
    written after a struct body, need no cases of their own.
    """
    # A definition: the name, optional dimensions, then `=`. A mere mention
    # cannot match, because a mention is not followed by an initialiser.
    site = re.compile(r"\b" + re.escape(symbol) + r"\b[^\S\n]*(?:\[[^\]]*\])*\s*=")
    for m in site.finditer(text):
        # Back to the start of the declaration: the previous `;` or `}`. A `{`
        # in between means this sits inside a function or another initialiser,
        # so it is not the definition we are looking for.
        at = m.start()
        while True:
            stop = max(text.rfind(";", 0, at), text.rfind("}", 0, at))
            head = text[stop + 1:at]
            if "{" in head:
                break
            word = re.search(r"\bstatic\b", head)
            if word is not None:
                cut = stop + 1 + word.start()
                text = text[:cut] + text[cut + len("static"):]
                # A forward declaration of the same symbol keeps its own
                # `static`, and internal linkage declared once cannot be
                # undeclared: "non-static declaration follows static
                # declaration". Both have to go.
                forward = re.compile(
                    r"((?:^|(?<=[;}]))[^\S\n]*)static\b([^;{}=]*\b"
                    + re.escape(symbol) + r"\b[^\S\n]*(?:\[[^\]]*\])*[^\S\n]*;)",
                    re.MULTILINE)
                return forward.sub(r"\1\2", text)
            # `static const struct { ... } sFoo[] =` puts the keyword in front
            # of a body of its own, so the `}` we stopped at is that body's.
            # Step over it to its opening brace and keep looking back.
            if stop < 0 or text[stop] != "}":
                break
            depth, i = 0, stop
            while i >= 0:
                if text[i] == "}":
                    depth += 1
                elif text[i] == "{":
                    depth -= 1
                    if depth == 0:
                        break
                i -= 1
            if i < 0:
                break
            at = i
    return None


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
