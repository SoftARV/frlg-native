#!/usr/bin/env python3
"""Remove a global data definition from a preprocessed source.

Globals only. A static cannot be declared away -- nothing can bind to a name
with internal linkage -- so those lose the `static` keyword instead and are
replaced by the linker; see tools/destatic.py and ADR 0020. This file used to
carry a `point_at_cart` that rewrote a static into a pointer into the cart, and
every shape of C it had to understand was a bug waiting to be found: the type,
the braces, the inner array dimensions, `sizeof`, anonymous structs, a storage
class written after the struct body. None of that is needed to delete a keyword.

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


def cut_definition(text, symbol, byte_size=0):
    """Turn `... symbol[...] = { ... };` into `extern ... symbol[...];`."""
    # Everything a declaration can put between the start of a line and the name:
    # type words, qualifiers and stars, in any order and any number -- `const u16
    # *const gLevelUpLearnsets[412]` needs the qualifier *after* the star, which
    # is the shape that catches a tidier pattern out.
    # preproc emits file-scope definitions run together on one line, so a
    # definition may begin right after the previous one's semicolon rather than
    # at the start of a line. Relaxing the anchor is safe because nothing is
    # rewritten unless the caller named the symbol.
    pattern = re.compile(
        r"(?:^|(?<=;))([^\S\n]*)((?:[A-Za-z_][A-Za-z0-9_]*\s+|\*+\s*)+)"
        # Dimensions optional: plenty of these are a single struct rather than
        # an array, and `extern const struct SpriteTemplate gFoo;` is the right
        # declaration for one.
        + re.escape(symbol) + r"\s*((?:\[[^\]]*\])*)\s*=\s*\{",
        re.MULTILINE)

    match = pattern.search(text)
    if match is None:
        return None

    indent, kind, dims = match.group(1), match.group(2).strip(), match.group(3)

    # `extern static const T sFoo;` is not valid C. A symbol the ROM's table
    # calls global but the source defines static cannot be declared away -- it
    # has to be pointed at the cart instead, like any other static.
    if kind.split()[0] == "static":
        return None

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
                #
                # An array written `gFoo[] = {...}` has no bound to keep, which
                # would leave an incomplete type. The ROM's symbol table knows
                # how many bytes it occupies and the compiler knows how big an
                # element is, so the declaration works out its own length --
                # `extern const T gFoo[1234 / sizeof(const T)]`. That is the
                # same number the definition had, and ARRAY_COUNT still works.
                bound = dims
                if bound.replace(" ", "") == "[]" and byte_size:
                    bound = f"[{byte_size} / sizeof({kind})]"

                return (text[:match.start()]
                        + f"{indent}extern {kind} {symbol}{bound};"
                        + text[end + 1:])
        i += 1
    return None


def cut_tentative(text, symbol):
    """Turn `const struct SpriteTemplate gFoo;` into a declaration too.

    A file-scope declaration with no initialiser and no `extern` is a *tentative
    definition*: on its own it allocates a zero-filled object. The
    decompilation has these -- field_effect_object_template_pointers.h declares
    every template it then takes the address of -- so replacing the initialised
    definition elsewhere is not enough. The tentative one silently becomes the
    definition, the symbol stays local, the cart binding never applies, and the
    game reads a structure full of zeros. It crashed on a null callback the
    moment a field effect was created.
    """
    # At column zero only. Indented means inside a function, where `return
    # gSomething;` has exactly this shape and turning it into
    # `extern return gSomething;` is not an improvement.
    pattern = re.compile(
        r"^((?:[A-Za-z_][A-Za-z0-9_]*[^\S\n]+|\*+[^\S\n]*)+)"
        + re.escape(symbol) + r"[^\S\n]*((?:\[[^\]]*\])*)[^\S\n]*;",
        re.MULTILINE)

    def replace(match):
        kind = match.group(1).strip()
        # `static` too: `extern static const T sFoo;` is not valid C, and a
        # static's tentative declaration is not ours to turn into an extern.
        if kind.split()[0] in ("extern", "return", "typedef", "static"):
            return match.group(0)
        return f"extern {kind} {symbol}{match.group(2)};"

    return pattern.sub(replace, text)


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: patch_data_definitions.py FILE SYMBOL [SYMBOL...]")

    path, symbols = sys.argv[1], sys.argv[2:]
    with open(path) as fh:
        text = fh.read()

    # Every refusal in one pass, not the first one. Reporting them one at a time
    # makes the caller rebuild once per symbol, and there are thousands.
    refused = []
    for symbol in symbols:
        byte_size = 0
        name = symbol
        if "@" in symbol:
            name, _, size_text = symbol.partition("@")
            byte_size = int(size_text)

        cut = cut_definition(text, name, byte_size)
        if cut is None:
            refused.append(name)
            continue
        text = cut_tentative(cut, name)

    if refused:
        for name in refused:
            print(f"patch_data_definitions: cannot rewrite {name} in {path}",
                  file=sys.stderr)
        sys.exit(1)

    with open(path, "w") as fh:
        fh.write(text)


if __name__ == "__main__":
    main()
