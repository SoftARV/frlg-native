# 0020 — The linker removes the game's data, not a text patch

**Status:** accepted
**Refines:** [ADR 0006](0006-rom-supplied-data.md), which decided *that* the binary ships no game
data. This decides *how*.

**Date:** 2026-08-15

## Context

ADR 0006's mechanism was to remove each definition from the preprocessed source: a global became a
declaration, and a static became a pointer into the cart image. The second half is where the cost
was. `point_at_cart` had to understand the C it was rewriting — the type, the brace nesting, the
inner array dimensions, `sizeof`, anonymous struct types, a storage class written after the struct
body — and each new shape arrived as a build failure rather than as a question.

Extracting one batch of 245 symbols needed six fixes to that script in a single evening, all of the
same kind: a regular expression parsing C, meeting C it had not met. Two of the six were bugs in the
fix before it.

Meanwhile the project already had everything needed to do it without touching source at all. The
linker binds 21,755 cart symbols by `--defsym`, and a `--defsym` **overrides an object's own
definition** rather than colliding with it.

## Decision

**Data leaves the binary at link time.**

- Game sources compile with `-fdata-sections`, so every variable has a section of its own.
- The existing `--defsym name=agb_cart+offset` bindings override the definitions.
- `--gc-sections` then drops the sections nothing references any more.

**Globals** need nothing else; their references already name the symbol.

**Statics** need one edit: the `static` keyword is deleted (`tools/destatic.py`). This is not
cosmetic. A static's references are compiled to `.rodata.sFoo + offset` — a *section* relocation, so
no `--defsym`, rename or `objcopy` can redirect them. Without `static` the same reference becomes
`R_386_GOTOFF sFoo`, which the linker can replace like any other.

**The five names defined in more than one file** — `sWindowTemplates`, `sBg_Tilemap`, `sBg_Gfx`,
`sStar_Gfx`, `sBg_Pal` — are renamed per file as well, since one `--defsym` cannot point a name at
seven addresses. A static is visible only in its own translation unit, so renaming it there is
complete; there is no reference elsewhere to miss. The list of shared names is written down rather
than discovered, so that a new collision is a build error and not a silent change of mechanism.

## Consequences

**`point_at_cart` is deleted** — 163 lines, and with them every shape of C it had to parse.
`destatic.py` is 89 lines and handled all six of the shapes that defeated its predecessor on the
first attempt, because it does not parse them: it deletes a keyword and leaves the declaration
exactly as it stands.

**`sizeof` and `ARRAY_COUNT` work by themselves.** The declaration keeps its type and bounds, so the
compiler still computes the right number. The old mechanism turned an array into a pointer, which
made `sizeof` silently wrong — the reason it had to refuse symbols, then substitute their sizes.

**Verified against the previous mechanism, not merely against itself**: the recorded play-throughs
render byte-identical frames, all seven run clean, the suite passes, and the residual-data scan
reports the same two benign matches — `gCrc16Table`, which is the standard CRC table our own code
computes, and one libc string that coincides.

**`--gc-sections` is now load-bearing.** A symbol reached only through the cart's own relocated
pointers survives because `agb_relocations.c` names it; that was the risk worth testing before
adopting this, and it held. Anything added later that is reachable *only* at runtime, and not named
by the relocation table, would be collected. That is the one new way to break this.

**The trial link that discovers unbound symbols now fails loudly** when it fails for want of a
toolchain rather than for want of symbols. It used to write an empty binding list, and the build
died hundreds of lines later with undefined references that looked exactly like this change's fault.
