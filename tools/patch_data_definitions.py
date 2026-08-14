#!/usr/bin/env python3
"""Remove a data definition from a preprocessed source, leaving its declaration.

ADR 0006 says the game's data comes from the player's ROM. Whole files of data
are simply not compiled (FRLG_GAME_DATA_ONLY), but most of the tables share a
translation unit with code that has to be compiled, so the definition has to go
without the file going with it.

Deleting it is enough: the decompilation already declares each of these in a
header -- `extern const struct Item gItems[];` in include/item.h -- so what is
left compiles, the symbol is unresolved, and the machinery that binds unresolved
symbols into the cart region takes it from there.

Exact-match and fails when a definition is absent, like the other patches in this
pipeline: a silently skipped one would ship the data it was meant to remove.
"""

import re
import sys


def cut_definition(text, symbol):
    """Turn `... symbol[...] = { ... };` into `extern ... symbol[...];`."""
    # Everything a declaration can put between the start of a line and the name:
    # type words, qualifiers and stars, in any order and any number -- `const u16
    # *const gLevelUpLearnsets[412]` needs the qualifier *after* the star, which
    # is the shape that catches a tidier pattern out.
    pattern = re.compile(
        r"^([^\S\n]*)((?:[A-Za-z_][A-Za-z0-9_]*\s+|\*+\s*)+)"
        # Dimensions optional: plenty of these are a single struct rather than
        # an array, and `extern const struct SpriteTemplate gFoo;` is the right
        # declaration for one.
        + re.escape(symbol) + r"\s*((?:\[[^\]]*\])*)\s*=\s*\{",
        re.MULTILINE)

    match = pattern.search(text)
    if match is None:
        return None

    indent, kind, dims = match.group(1), match.group(2).strip(), match.group(3)

    # Brace matching from the opening brace, skipping strings and characters so
    # that a brace inside them cannot end the definition early.
    at = text.index("{", match.end() - 1)
    depth, i, n = 0, at, len(text)
    while i < n:
        c = text[i]
        if c == '"' or c == "'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                end = text.find(";", i)
                if end == -1:
                    return None
                # A declaration rather than nothing, and one that keeps the
                # bounds: some of these have no extern in any header, and code
                # that takes ARRAY_COUNT of one needs the size to survive.
                return (text[:match.start()]
                        + f"{indent}extern {kind} {symbol}{dims};"
                        + text[end + 1:])
        i += 1
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: patch_data_definitions.py FILE SYMBOL [SYMBOL...]")

    path, symbols = sys.argv[1], sys.argv[2:]
    with open(path) as fh:
        text = fh.read()

    for symbol in symbols:
        cut = cut_definition(text, symbol)
        if cut is None:
            sys.exit(f"patch_data_definitions: no definition of {symbol} in {path}")
        text = cut

    with open(path, "w") as fh:
        fh.write(text)


if __name__ == "__main__":
    main()
