# ADR 0008 — Golden screenshots, headless drivers, one regression per bug

**Status:** accepted

## Context

320k lines of game logic will never be read by anyone on this project, and they are not where the
bugs will be. The bugs will be in the hardware layer, where a one-line mistake in a blend rule or a
DMA timing is invisible in code review and obvious on screen.

We also have something most projects do not: an oracle. The ROM builds, so "what is correct" is
always answerable by running the real thing.

`gen1recomp` arrived at three practices worth copying wholesale. Their suite is ~190 files, of
which roughly 150 are named after the bug they prevent — `battle_colors_bug316_test.lua`,
`bag_full_pickup_bug872_test.lua`.

## Decision

Three tiers, plus a discipline.

**Unit tests** for anything with a knowable right answer: BIOS arithmetic against hardware-derived
vectors, decompressors round-tripping real assets.

**Golden screenshots** compared with two independent thresholds, which is the detail that makes
them survivable:

- a **per-channel tolerance** absorbing harmless drift between GPUs and driver versions
- a separate **pixel budget** for how many pixels may differ at all, which is what actually fails

A shot where one sprite moved trips the budget even though every differing pixel is far outside the
tolerance; a shot that got globally half a shade darker trips neither. Every regression writes a
side-by-side image as a CI artifact, and `--bless` updates goldens deliberately.

**Headless drivers** — scripted scenarios run against the `null` backend, plus an autopilot
playthrough capturing shots at fixed points. Our fiber design (ADR 0004) makes these deterministic
frame for frame, which is the property that makes the whole tier possible.

**The discipline: every fixed bug gets a driver named after it**, committed with the fix.

## Consequences

- Refactoring the hardware layer becomes possible, because "did this change anything" is a hash
  comparison rather than an afternoon of playing.
- Goldens need blessing discipline. A reviewer who blesses a hundred shots without reading the
  diffs converts the whole tier into decoration.
- The software PPU (ADR 0005) makes rendering deterministic across machines, so goldens are stable
  in CI. A GPU-side PPU would have made this tier far weaker.
- CI cost grows with platform count, so expensive per-platform jobs are gated behind
  change-detection jobs, and SDK-free self-tests run everywhere.

## Alternatives rejected

**Golden images with a single threshold** — either too tight to survive a driver update, or too
loose to catch a sprite moving.

**Manual play-testing** — does not scale to 7 platforms, and cannot answer whether a hardware-layer
refactor changed behaviour.
