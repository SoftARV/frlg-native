# ADR 0010 — Golden images are generated per machine, never committed

**Status:** accepted

## Context

[ADR 0008](0008-testing-strategy.md) makes golden screenshots a tier of the test strategy, and
almost every project that has such a tier commits the reference images beside the code. That is
what makes them useful: clone, build, run, and the comparison works.

We cannot do that. A golden screenshot of this game is a frame of a copyrighted ROM — the tiles,
the palettes, the sprites and the text are all game data. [ADR 0006](0006-rom-supplied-data.md)
says game data comes from the player's own ROM and that we ship none, and
[required-to-function.md](../required-to-function.md) states the same to the player. A directory of
title screens in the repository would contradict both, and it would do so in the most visible way
available: as images, in a public tree.

The `shots/` directory has been gitignored for this reason since the first frame rendered. The
question this settles is what the golden tier does about it, rather than whether the rule applies.

## Decision

**Goldens are generated on the machine that runs the tests, and never committed.**

- `tests/golden/manifest.txt` **is** committed: which frames to capture, the two thresholds for
  each, and what the frame is. It is a few lines of text and contains no game data.
- `tests/golden/images/` is gitignored. `tools/golden.py --bless` fills it from a build the
  developer trusts.
- A run with no goldens fails with a message naming `--bless`, rather than silently passing. An
  absent reference is an unproven frame, and the tier is worthless if that reads as success.

## Consequences

- **The tier catches regressions, not incorrectness.** Blessing our own renderer proves only that
  it has not changed. Correctness needs a reference from outside — mGBA frame dumps of the same
  ROM, which is the second half of the phase 3 milestone and is not built yet. Until then a green
  golden run means "the same as yesterday", and the manifest says so.
- **Blessing is a judgement, not a chore.** Whoever runs `--bless` is asserting that what the port
  renders now is right. ADR 0008 already warns that a reviewer who blesses a hundred shots without
  reading the diffs converts the tier into decoration; generated goldens make that risk sharper,
  because there is no committed baseline for a reviewer to compare against.
- **CI has to bless before it can compare**, from its own ROM build, and cannot cache goldens
  across ROM revisions. That is a cost, and it is the price of the shipping story.
- Nothing about the harness changes when mGBA references arrive: they are blessed into the same
  directory, and only the thresholds in the manifest need raising.

## Alternatives rejected

**Commit the goldens.** Ships game data. Not available to us at any price.

**Commit hashes instead of images.** A hash is not imagery and could be committed, but it collapses
the two thresholds into one bit. A frame half a shade darker and a frame with a sprite in the wrong
place become the same failure, and neither produces a diff anyone can read. That is precisely the
single-threshold design ADR 0008 rejected.

**Commit goldens rendered from placeholder art.** They would compare against nothing the game
actually draws, so every real frame would fail. A reference has to be the thing being referenced.
