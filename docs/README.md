# Documentation

| Document | Read it when |
| --- | --- |
| [ARCHITECTURE.md](ARCHITECTURE.md) | You want to know how any part of the project works |
| [ROADMAP.md](ROADMAP.md) | You want to know what works today and what comes next |
| [BUILDING.md](BUILDING.md) | You want to build it |
| [required-to-function.md](required-to-function.md) | You want to know what the app needs from the player, and what it ships |
| [new-features.md](new-features.md) | You want to know what this port does that the cartridge did not |
| [adr/](adr/) | You want to know *why* something is the way it is |

There is deliberately no "known differences" document. Unlike a reimplementation, this port runs
the original code, so faithful behaviour is not an achievement to track — it is the default.

## Policy

`ARCHITECTURE.md` is the contract, not a summary written after the fact.

- A commit that adds, removes or changes the behaviour of a subsystem, an interface or the build
  **updates `ARCHITECTURE.md` in the same commit.** Not a follow-up commit.
- A bugfix inside an already-documented subsystem does not need a doc change.
- Adding an override without adding its row to the override table is a defect, not an oversight.
- Decisions that close off an alternative get an ADR. ADRs are immutable once accepted — if a
  decision changes, write a new ADR that supersedes the old one and say so in both.

The reason for the strictness: the game layer is 320k lines nobody will read, and the hardware
layer is where every bug will actually live. Documentation that drifts from the hardware layer
makes the project unmaintainable within months.

## ADRs

| # | Decision |
| --- | --- |
| [0001](adr/0001-source-port.md) | Source port, not emulation or recompilation |
| [0002](adr/0002-upstream-submodule.md) | Upstream as a pinned submodule with shadow headers and overrides |
| [0003](adr/0003-pointer-width.md) | 32-bit first, 64-bit ready |
| [0004](adr/0004-fiber-frame-loop.md) | The game runs on a fiber |
| [0005](adr/0005-sdl3-software-ppu.md) | SDL3 backend, software PPU |
| [0006](adr/0006-rom-supplied-data.md) | Game data comes from the player's ROM |
| [0007](adr/0007-lua-mod-registries.md) | Embedded Lua with schema-driven registries |
| [0008](adr/0008-testing-strategy.md) | Golden screenshots, headless drivers, one regression per bug |
