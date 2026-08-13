# ADR 0015 — This is an enhanced port, not a preservation port

**Status:** accepted

## Context

The documentation grew up around a fidelity argument, and the argument is good: the game's own C is
what runs, so battle formulas, trainer AI, encounter rates and the original's quirks are correct by
construction rather than by effort. Every tool built so far serves it. The mGBA oracle exists to
answer "does this match the ROM", the golden tier compares frames against it, and
`docs/spikes/0007-audio-against-mgba.md` is a long argument about matching a reference spectrum.

That framing then leaked into the *defaults*. `ARCHITECTURE.md` 4.2 opens with "Overrides are a cost,
not a convenience". A change that made the port differ from the cartridge was being written up as
something to justify, and the first such change — defaulting a new save to stereo, `a57a9ad` — was
hedged in its own commit message and its own doc row as though it needed an excuse.

It does not. The point of compiling this game for the host CPU is that the host is not a Game Boy
Advance. The 240×160 screen, the 13379 Hz mixer, the 8-bit sound buffer, the 256 KB of EWRAM and the
16.78 MHz ARM7 are not requirements; they are what the original was affordable within. Most of the
game will stay as it is, and a lot of it will not.

## Decision

**Deliberate divergence from the original is the project's purpose, not a debt against it.**

- Enhancements need no fidelity justification. Resolution, audio bandwidth, load times,
  quality-of-life defaults and anything else the cartridge could not afford are in scope on their
  merits.
- What stays is the *game*: its own code keeps running, so its rules and its quirks continue to hold
  unless something deliberately changes them.
- The distinction that matters is **deliberate** divergence against **accidental** divergence. The
  first is a feature and gets recorded. The second is a bug and gets fixed.

This does not retire the oracle — it is what makes the distinction checkable. mGBA tells us what
differs from the original; this ADR says the answer to "why is this different" may simply be "because
we chose to", and that answer is complete.

## Consequences

The cost that remains in an override is a **maintenance** cost, not a fidelity one: a forked file
stops receiving upstream fixes. That is still a real reason to prefer a macro seam, and 4.2's
preference stands on those grounds alone.

The golden tier will eventually fail on purpose. A test that asserts "matches mGBA frame for frame"
is asserting the absence of exactly the changes this project intends to make. Each enhancement that
alters output has to say what it expects to break, and the affected goldens are regenerated with the
reason recorded — the same discipline `ADR 0010` already sets for generated goldens. An enhancement
that quietly moves a golden without saying so is indistinguishable from a bug.

Deviations are recorded where the project already records things: an ADR when the decision closes off
alternatives, a row in the prelude or override table when it is a build seam, `ARCHITECTURE.md` when
it changes a subsystem's contract. "Recorded" is the only obligation an enhancement carries.

The README's framing needs to follow. It currently sells fidelity alone, which undersells what the
project is and misleads anyone reading it cold.
