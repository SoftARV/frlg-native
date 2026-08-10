# ADR 0011 — Enhancements layer over a preserved reference configuration

**Status:** accepted

## Context

The port now has an external oracle. [Spike 0004](../spikes/0004-mgba-frame-alignment.md) showed
that mGBA running our own reference ROM build reproduces the port exactly — 0 of 38,400 pixels on
the title screen — which is what turns a golden from "matches yesterday" into "matches the ROM".

That oracle is only meaningful while the port renders what a Game Boy Advance renders, and the
roadmap is explicitly heading somewhere else. Phase 9 gives mods display pipelines
([ADR 0007](0007-lua-mod-registries.md)); phase 12 adds widescreen, higher internal resolution and
high refresh ([ARCHITECTURE §13](../ARCHITECTURE.md#13-extension-points)). Each deliberately
changes the picture. The obvious reading is that the comparison is a scaffold for the early phases
and expires when the port starts being a PC port rather than a reproduction.

It does not have to expire — but only if the port can still be *asked* for a GBA-equivalent frame.
Two decisions already in place keep that frame well-defined whatever is layered above it: game
logic believes the screen is 240×160 and is never told otherwise, and logic stays locked to
59.7275 Hz with enhancement frames interpolated between ticks. "One logic tick, at native
resolution" therefore remains a meaningful thing to ask for at any phase.

The risk is not feasibility, it is implementation shape. If an enhancement *replaces* the base path
— the renderer only ever emitting widescreen, the frame loop only ever emitting interpolated frames
— then the reference frame becomes unproducible, and recovering the oracle later means unpicking
the enhancement work. This is the same shape as [spike 0002](../spikes/0002-host-assembly.md) and
[spike 0003](../spikes/0003-empty-cart-region.md): an assumption that costs almost nothing to
protect now and a phase to discover late.

## Decision

**Every enhancement is a layer over a reference configuration the port can always produce.**

The reference configuration is: native 240×160, one frame per logic tick, no interpolation, no
mods, no display pipeline. It is the configuration the golden tier's referenced frames are captured
in, and the one the conformance comparison against mGBA runs in.

- Enhancements are **switchable off, not baked in**. A build that cannot produce a reference frame
  is a defect, not a variant.
- Where an enhancement is geometric and additive, **conformance survives by cropping**: widescreen
  at native scale leaves the centre 240×160 identical, because the extra area is overdraw around a
  screen the game still believes is 240×160.
- Where it is not — upscaling, interpolation, mods — the frame simply **carries no reference** in
  `tests/golden/manifest.txt`, exactly as frames 400 and 900 do today for an unrelated reason.

## Consequences

- **Conformance testing survives the whole roadmap.** The title screen can still be compared
  against the ROM at phase 12, which is when the port has diverged most and an external check is
  worth most.
- **Widescreen gains a free correctness check.** Its cropped centre must still match mGBA, which
  distinguishes an enhancement that added to the picture from one that disturbed it.
- **Every enhancement costs a switch.** This rules out the shortest implementation of several of
  them — changing `SCREEN_W` and being done — and that is the price being paid deliberately.
- **Mod display modes can never be conformance-tested**, by definition. That is correct: a mod that
  changed nothing observable would not be a mod. They get regression goldens and nothing more.
- **Pixels are not the only comparison available.** Game state — the RNG stream, save blocks, memory
  — is untouched by rendering enhancements, so the headless-driver tier can be checked against mGBA
  even in configurations where no pixel oracle exists. That is not built, and this decision is what
  keeps it possible.

## Alternatives rejected

**Let the oracle expire at phase 9.** Cheapest today, unrecoverable later. It would drop the only
external check on the port at precisely the point where the port stops being verifiable by
inspection, and where "did this enhancement break the game underneath it" becomes the question that
matters most.

**Keep a separate reference build.** A second configuration existing only for tests. It would drift
from the shipping one — nothing forces them to agree — and conformance of a build nobody runs
proves very little about the build everybody runs.

**Compare against mGBA only until phase 8.** The same decision as letting it expire, with a date
attached to make it feel planned.
