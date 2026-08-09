# ADR 0002 — Upstream as a pinned submodule with shadow headers and overrides

**Status:** accepted

## Context

The port needs to change how the game reaches hardware, without owning 320k lines of code that
pret is still actively fixing. A fork would let us edit anything, at the price of merge conflicts
forever, and of never being able to answer "what did we actually change" without a diff against an
ever-moving upstream.

The measurements that made the strict version viable: hardware access funnels through five headers
in `include/gba/`, and only 38 raw address literals exist in all of `src/`.

## Decision

`vendor/pokefirered` is pinned and never edited. Deviations take exactly three forms:

1. **Shadow headers** — our `include/` precedes upstream's; five hardware headers are replaced.
2. **Overrides** — upstream `.c` files swapped at build time, each one listed in the override table
   in `docs/ARCHITECTURE.md`.
3. **Exclusions** — the five `.s` files, which describe a cartridge we are not.

## Consequences

- "What did we change" is always answerable: five headers, one table, five exclusions.
- Upstream bug fixes arrive by bumping a pin.
- Shadowing is invisible at the call site, so upstream edits to a shadowed header could silently
  diverge. `tools/check_drift.py` pins the upstream hash of every shadowed and overridden file and
  fails CI when one moves. This check is what makes the approach safe rather than clever.
- Overrides lose upstream fixes for that file, so they stay rare and justified.
- Build machinery is more complex than a fork's — the cost we chose to pay.

## Alternatives rejected

**Fork with history** — simplest build, permanent merge tax, port and game code intermixed.

**Vendored snapshot** — maximum freedom, permanently forfeits upstream fixes.
