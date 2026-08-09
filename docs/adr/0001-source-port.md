# ADR 0001 — Source port, not emulation or recompilation

**Status:** accepted

## Context

The goal is a multiplatform port that can be enhanced later. Three techniques were available.

The decompilation is functionally complete: 320k lines of C, with only five `.s` files remaining,
all of them boot, BIOS-ABI or sound-mixer glue.

## Decision

Compile the game's C directly for the host CPU and reimplement the hardware beneath it.

## Consequences

- Enhancements become ordinary software changes. Widescreen is a renderer parameter, not a hack
  against a frozen binary.
- We take on reimplementing the PPU, DMA, interrupts, BIOS and sound mixer — the real cost.
- The game source stays pristine and keeps receiving upstream fixes.
- Performance is native; the port is not fill-rate or CPU bound at this scale.

## Alternatives rejected

**Emulation.** Shipping a CPU interpreter gets a running game fastest and then blocks every goal
that follows: enhancement means patching machine code.

**Static recompilation.** N64Recomp-style lifting exists to serve games *without* source. Using it
here would mean discarding 320k lines of readable C to recover something strictly worse.
