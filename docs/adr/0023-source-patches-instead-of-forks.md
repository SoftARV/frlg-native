# 0023 — Restructuring upstream is a diff, not a fork

**Status:** accepted
**Date:** 2026-08-15

**Refines** [ADR 0022](0022-port-code-in-the-game-layer.md), which said *patch to insert, fork to
restructure*. The second half was wrong, and this replaces it.

## Context

The port's pause-menu entry could not be added by insertion. The menu window is built at a fixed base
block with the dialogue box directly above it — `0x13D`, and `0x198` — leaving 91 tiles. Seven
entries fill them exactly; an eighth needs 105 and writes over the box the help text is rendered
into, which is a crash. A save with the Pokédex and Pokémon unlocked already has seven entries
without ours.

Making the list *scroll* means replacing function bodies — how items are printed, how the cursor
moves — and an anchored insertion cannot do that. ADR 0022 said fork, and
[§4.2](../ARCHITECTURE.md#42-overrides) defines a fork as copying the upstream file here and
compiling ours instead.

**That copy is the problem.** `start_menu.c` is a thousand lines of pret's decompiled code, and the
README says plainly that `vendor/pokefirered` "is fetched by whoever builds rather than redistributed
here". Committing the file would make that false — in a repository that had just been licensed
GPL-3.0, over a menu. The override table has always read *(none yet)*, so nothing had yet tested the
claim.

## Decision

**A change that restructures an upstream file is a unified diff, applied at build time.**

`platform/game/overrides/<name>.patch` is applied to a copy of the vendor source in the build tree;
the submodule is never written to. The patched copy then goes through `cpp` and `preproc` like any
other source.

This keeps what the fork was for and drops what it cost:

- **Restructuring** — a diff replaces whole function bodies, which is the thing insertions cannot do.
- **No redistribution.** The repository holds our changes and the few lines of context a diff needs
  to locate them, not the file.
- **Loud on drift.** A patch that no longer applies fails the build, and `check_drift.py` watches the
  file besides.

Applied with `--forward`, so re-running against an already-patched copy is an error rather than a
silent reversal.

## Consequences

**`platform/agb/overrides/` stays empty**, and the override table with it. If a genuine fork is ever
needed — a file that cannot be expressed as a diff at all — it is a deliberate decision that changes
the README, not a side effect of a feature.

**A diff is harder to read than a file.** You cannot open it and see the menu; you see what changed
about the menu. That is the real cost and it is accepted: the alternative was a thousand lines that
silently stop receiving upstream fixes, where a diff of the same change is two hundred and says
exactly what it did.

**A submodule bump may need the diff rebased by hand.** `patch` will refuse rather than guess, which
is the correct failure. `check_drift.py` reports the move first, so the rebase is expected rather
than discovered in a build log.

**Three mechanisms now exist, in order of preference:** a shadow macro in the prelude, an anchored
insertion, a diff. Reach for the next one only when the one before it cannot express the change. The
fourth — copying a file here — remains available and remains a decision about what this repository
distributes, not a technique.

## Alternatives

**Fork `start_menu.c`.** Rejected: it puts pret's code in this repository and contradicts the licence
section written the same day.

**Narrow the menu window from seven tiles to six**, which fits eight entries in 90 of the 91 tiles.
One line, and it would have worked today — but it spends the last tile of a budget that is already
exactly full, and the next entry has nowhere to go. Scrolling is the answer that survives the entry
after this one.

**Put the port's options in the existing OPTION screen.** Still reasonable, and it needs no budget at
all. Rejected because the pause menu is where a player looks for what a program can do, and because
the scrolling list is needed the moment any second entry is added anyway.
