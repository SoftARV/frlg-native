# ADR 0006 — Game data comes from the player's ROM

**Status:** accepted
**Supersedes part of:** [0002](0002-upstream-submodule.md) §"files simply not built"

## Context

The decompilation ships the game's assets: 3048 PNGs (55 MiB of graphics), 16 MiB of sound and
26 MiB of data, all derived from the copyrighted ROM. A build that `.incbin`s them is a
redistribution of Nintendo's work, which makes every binary undistributable and confines the
project to source-only forever.

`gen1recomp` solved this by shipping a manifest of addresses and symbolic names containing no ROM
bytes, and decoding everything from the player's own cartridge dump at first boot. They had to
reverse-engineer that manifest by hand.

We do not. We build the ROM ourselves, so the manifest is a build output: `make syms` emits 50,590
symbols with addresses and sizes (9,832 of them `g`-prefixed data symbols), beside a 35,037-line
linker map. We know exactly where every byte of data lives in a legitimate ROM.

## Decision

**The binary contains code and a generated manifest. It contains no game data.**

At first boot the player supplies a ROM. It is SHA-1 verified against the revisions we support,
loaded into the cart region of the memory arena, and released — never copied into the cache.

The game's data symbols are not compiled in. A generated linker fragment *defines* each one at its
address inside the cart region, so `extern const struct SpeciesInfo gSpeciesInfo[]` resolves into
the loaded ROM image and game code reaches its data exactly as before, unmodified.

Pointers embedded in that data are ROM addresses (`0x08xxxxxx`). The importer relocates them in
place to host addresses, driven by a relocation table derived from the ROM build. This is the same
mechanism ADR 0003 needs for embedded pointers, and it is why the two decisions share one seam.

**A developer data path exists in parallel**: a dev build compiles the data in, as upstream does,
so renderer work can proceed before the importer is finished. It is never distributed.
`gen1recomp` keeps the same split (`tools/build_data.py`), and it is what keeps the importer off
the critical path for Phases 1–3.

## Consequences

- Binaries become distributable, which is what makes a launcher, packaging and a release pipeline
  worth building at all.
- The manifest is mechanically generated and regenerates whenever the submodule pin moves, so it
  cannot rot the way a hand-written one would.
- Text, dialogue and every data table are covered automatically, because they live in the ROM
  image. This is what makes full extraction tractable for a source port at all — we are not
  externalising 9,832 symbols one at a time, we are pointing them at a file.
- The player must supply a specific supported revision. Unsupported dumps are rejected rather than
  decoded at wrong addresses.
- 64-bit still requires regenerating data into a native layout, since relocated host pointers do
  not fit the ROM's 4-byte slots. Unchanged from ADR 0003, Phase 8.

## Risks

- **Relocation table derivation** needs `--emit-relocs` on the ROM link, or reading relocation
  sections from the object files. Unproven here; it is the first spike of Phase 1.
- **The web target** may not support defining symbols at absolute addresses the way ELF does.
  Emscripten may need a pointer-indirection layer instead. Unresolved, and deliberately deferred —
  it affects Phase 7 only.

## Alternatives rejected

**Bake assets in** — simplest, and permanently source-only.

**Hybrid: import only the binary blobs** — removes most of the payload but still ships compiled-in
text and data tables, so it fails the same test for a weaker result.
