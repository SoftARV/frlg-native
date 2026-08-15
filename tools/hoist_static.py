#!/usr/bin/env python3
"""Lift a function-scope static out to file scope, so the linker can replace it.

`destatic` cannot help here. Inside a function the `static` keyword is what
makes the table persist; delete it and the table becomes an automatic variable
rebuilt on every call, which changes what the code does. The declaration has to
move instead -- out of the function, keeping `static`'s storage but losing its
linkage -- and then it is an ordinary file-scope definition that a --defsym
binding can override like any other. See ADR 0020.

The new name is given by the caller, because two functions in one file may each
have a `nibble` and file scope has room for only one.

Only references inside the enclosing function are renamed: another function's
identically named local is none of our business.

usage: hoist_static.py FILE NAME=NEWNAME [NAME=NEWNAME...]
"""
import re
import sys


def depth_map(text):
    """Brace depth before each position, ignoring braces in strings."""
    depth, out, i, n = 0, bytearray(len(text)), 0, len(text)
    while i < n:
        c = text[i]
        if c in "\"'":
            quote, i = c, i + 1
            while i < n and text[i] != quote:
                i += 2 if text[i] == "\\" else 1
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
        out[i] = min(depth, 255)
        i += 1
    return out


def hoist(text, name, newname):
    depth = depth_map(text)
    decl = re.compile(r"(?:^|(?<=[;{}]))([^\S\n]*)static\s+((?:[A-Za-z_]\w*\s+|\*+\s*)+)"
                      + re.escape(name) + r"\s*((?:\[[^\]]*\])*)\s*=\s*\{", re.MULTILINE)
    for m in decl.finditer(text):
        if depth[m.start()] < 1:
            continue                       # already at file scope
        # End of the initialiser.
        at = text.index("{", m.end() - 1)
        d, i, n = 0, at, len(text)
        while i < n:
            if text[i] == "{":
                d += 1
            elif text[i] == "}":
                d -= 1
                if d == 0:
                    break
            i += 1
        end = text.find(";", i)
        if end == -1:
            return None
        kind, dims = m.group(2).strip(), m.group(3)
        body = text[at:i + 1]

        # The enclosing function: back to where depth was last zero, which is
        # its opening brace -- then further back past the signature, to the end
        # of whatever came before it. Inserting at the brace would land the
        # definition between the signature and the body.
        start = m.start()
        while start > 0 and depth[start] > 0:
            start -= 1
        start = max(text.rfind(";", 0, start), text.rfind("}", 0, start)) + 1
        fin = i
        while fin < n and depth[fin] > 0:
            fin += 1

        moved = f"{kind} {newname}{dims} = {body};\n"
        inner = text[m.start():end + 1]
        region = text[start:fin + 1].replace(inner, "")
        region = re.sub(r"\b" + re.escape(name) + r"\b", newname, region)
        return text[:start] + moved + region + text[fin + 1:]
    return None


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: hoist_static.py FILE NAME=NEWNAME [NAME=NEWNAME...]")
    path = sys.argv[1]
    with open(path, encoding="utf-8", errors="surrogateescape") as fh:
        text = fh.read()

    missed = []
    for arg in sys.argv[2:]:
        name, _, newname = arg.partition("=")
        if not newname:
            sys.exit(f"hoist_static: {name} needs a new name")
        out = hoist(text, name, newname)
        if out is None:
            missed.append(name)
        else:
            text = out

    if missed:
        for name in missed:
            print(f"hoist_static: no function-scope static {name} in {path}",
                  file=sys.stderr)
        return 1

    with open(path, "w", encoding="utf-8", errors="surrogateescape") as fh:
        fh.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
