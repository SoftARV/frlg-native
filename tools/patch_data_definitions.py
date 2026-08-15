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


class _Span:
    """A match-like span, for the anonymous-struct case found by scanning."""

    def __init__(self, start, end, kind, dims):
        self._start, self._end, self._kind, self._dims = start, end, kind, dims

    def start(self):
        return self._start

    def end(self):
        return self._end

    def group(self, n):
        # (whole, indent, kind, dims) -- the caller reads 1, 2 and 3.
        return ("", "", self._kind, self._dims)[n]


def point_at_cart(text, symbol, offset, byte_size=0):
    """Replace a static's definition with a pointer into the imported image.

    A file-scope static has no name another translation unit can bind to, so it
    cannot be declared away like a global -- and statics are where most of the
    game's data still lives. What it can be is a pointer: the bytes are already
    in the cart image at a known offset, so

        static const struct Foo sBar[] = { ... };

    becomes

        static const struct Foo *const sBar = (const struct Foo *)(agb_cart + 0x1234);

    Indexing, `&sBar[0]` and passing it along all still compile and mean the same
    thing. `sizeof` does not: it silently becomes the size of a pointer, so
    ARRAY_COUNT would quietly compute the wrong number. That is refused here
    rather than guarded at run time, because a wrong count is exactly the kind of
    fault that looks like something else entirely.
    """
    # `sizeof(sBar)` on a pointer is four, so ARRAY_COUNT would quietly compute
    # the wrong number -- and ARRAY_COUNT is where most of these appear, since
    # the macro expands to sizeof(x) / sizeof((x)[0]) before this ever runs.
    #
    # Given the symbol's real size, the whole-array sizeof is replaced with that
    # literal and ARRAY_COUNT keeps working: the divisor `sizeof((sBar)[0])` is
    # the element type either way, and its text does not match this pattern.
    # Without a size there is nothing honest to substitute, so it is still
    # refused -- a wrong count is exactly the fault that looks like another one.
    whole_sizeof = re.compile(r"sizeof\s*\(\s*" + re.escape(symbol) + r"\s*\)")
    if whole_sizeof.search(text):
        if not byte_size:
            return None
        text = whole_sizeof.sub(f"({byte_size}u)", text)

    # A forward declaration of the same static -- `static const T sFoo[];`
    # ahead of its definition -- would contradict the pointer this becomes, and
    # the compiler reports it against the declaration rather than here.
    if re.search(r"^[^\S\n]*static[^\S\n][^;{=\n]*\b" + re.escape(symbol)
                 + r"[^\S\n]*(?:\[[^\]]*\])*[^\S\n]*;", text, re.MULTILINE):
        return None

    pattern = re.compile(
        r"(?:^|(?<=;))([^\S\n]*)((?:[A-Za-z_][A-Za-z0-9_]*[^\S\n]+|\*+[^\S\n]*)+)"
        # The brace may sit on the next line, which several of these do.
        + re.escape(symbol) + r"[^\S\n]*((?:\[[^\]]*\])+)\s*=\s*\{",
        re.MULTILINE)
    match = pattern.search(text)
    if match is None:
        # An anonymous struct type -- `static const struct { ... } sFoo[] = {`.
        # There is no type name to cast to, but none is needed: the declaration
        # keeps the type text exactly as it stands and takes its value from a
        # void pointer, which converts implicitly.
        # The struct body has semicolons of its own, so this is found by
        # locating `} sFoo[...] = {` and walking back to the brace that opens
        # the body, then to the `static` in front of it.
        # text.c writes the storage class after the struct body:
        # `struct { ... } static const sKeypadIcons[] = {`.
        head = re.search(r"\}[^\S\n]*((?:(?:static|const|volatile)[^\S\n]+)*)"
                         + re.escape(symbol)
                         + r"[^\S\n]*((?:\[[^\]]*\])+)\s*=\s*\{", text)
        if head is None:
            return None
        depth, i = 0, head.start()
        while i >= 0:
            if text[i] == "}":
                depth += 1
            elif text[i] == "{":
                depth -= 1
                if depth == 0:
                    break
            i -= 1
        if i < 0:
            return None
        # The declaration begins after the previous statement, not at the
        # struct keyword: `static const struct { ... }` must be replaced whole,
        # or its qualifiers are left stranded in front of the replacement.
        kw = max(text.rfind("struct", 0, i), text.rfind("union", 0, i))
        if kw == -1:
            return None
        stop = max(text.rfind(";", 0, kw), text.rfind("}", 0, kw),
                   text.rfind("\n", 0, kw))
        start = stop + 1
        while start < kw and text[start] in " \t\n":
            start += 1
        # Whatever qualifiers followed the body belong in front of the type.
        trailing = head.group(1).strip()
        kind = text[start:head.start() + 1]
        if trailing:
            kind = trailing + " " + kind
        match = _Span(start, head.end(), kind, head.group(2))
        return _rewrite(text, match, symbol, offset, anonymous=True)
    return _rewrite(text, match, symbol, offset, anonymous=False)


def _rewrite(text, match, symbol, offset, anonymous):

    indent, kind, dims = match.group(1), match.group(2).strip(), match.group(3)
    if anonymous:
        # The type text is kept verbatim -- it is the struct body -- and the
        # pointer takes its value from a void pointer rather than a cast to a
        # name that does not exist.
        base = kind[len("static"):].strip() if kind.startswith("static") else kind.strip()
    else:
        words = kind.split()
        if words[0] != "static":
            return None                  # not ours to rewrite
        base = " ".join(words[1:])

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
                # Only the outermost dimension is the one being replaced by a
                # pointer. `sFoo[][3]` indexed as sFoo[a][b] must stay a pointer
                # *to an array of 3*, or the inner subscript has nothing to
                # apply to -- which the compiler reports somewhere else entirely.
                inner = "".join(re.findall(r"\[[^\]]*\]", dims)[1:])
                if inner:
                    return (text[:match.start()]
                            + f"{indent}static {base} (*const {symbol}){inner} = "
                              f"({base} (*){inner})(agb_cart + {offset:#x});"
                            + text[end + 1:])

                cast = "(const void *)" if anonymous else f"({base} *)"
                return (text[:match.start()]
                        + f"{indent}static {base} *const {symbol} = "
                          f"{cast}(agb_cart + {offset:#x});"
                        + text[end + 1:])
        i += 1
    return None


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
        # `name#offset` is a static, which becomes a pointer into the cart
        # rather than a declaration -- nothing can bind to a static's name.
        if "#" in symbol:
            name, _, offset_text = symbol.partition("#")
            # `name#offset@size` carries the symbol's size as well, which is what
            # lets a whole-array sizeof survive the rewrite.
            offset_text, _, size_text = offset_text.partition("@")
            pointed = point_at_cart(text, name, int(offset_text, 0),
                                    int(size_text) if size_text else 0)
            if pointed is None:
                refused.append(name)
            else:
                text = pointed
            continue

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
